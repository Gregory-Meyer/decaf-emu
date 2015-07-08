#include "coreinit.h"
#include "coreinit_thread.h"
#include "coreinit_scheduler.h"
#include "coreinit_systeminfo.h"
#include "memory.h"
#include "processor.h"
#include "trace.h"
#include "system.h"
#include <Windows.h>

static OSThread *
gDefaultThreads[3];

static uint32_t
gThreadId = 1;

static void
__OSSleepThreadNoLock(OSThreadQueue *queue)
{
   auto thread = OSGetCurrentThread();
   thread->queue = queue;
   thread->state = OSThreadState::Waiting;

   if (queue) {
      OSInsertThreadQueue(queue, thread);
   }
}

static void
__OSWakeupThreadNoLock(OSThreadQueue *queue)
{
   for (auto thread = queue->head; thread; thread = thread->link.next) {
      gProcessor.queue(thread->fiber);
   }
}

static void
__OSWakeupThreadWaitForSuspensionNoLock(OSThreadQueue *queue, int32_t suspendResult)
{
   for (auto thread = queue->head; thread; thread = thread->link.next) {
      thread->suspendResult = suspendResult;
      gProcessor.queue(thread->fiber);
   }
}

static int32_t
__OSResumeThreadNoLock(OSThread *thread, int32_t counter)
{
   auto old = thread->suspendCounter;
   thread->suspendCounter -= counter;

   if (thread->suspendCounter < 0) {
      thread->suspendCounter = 0;
      return old;
   }

   if (thread->suspendCounter == 0) {
      if (thread->state == OSThreadState::Ready) {
         gProcessor.queue(thread->fiber);
      }
   }

   return old;
}

static void
__OSSuspendThreadNoLock(OSThread *thread)
{
   thread->requestFlag = OSThreadRequest::None;
   thread->suspendCounter += thread->needSuspend;
   thread->needSuspend = 0;
   thread->state = OSThreadState::Ready;
   __OSWakeupThreadNoLock(&thread->suspendQueue);
}

static void
__OSRescheduleNoLock()
{
   gProcessor.reschedule(true);
}

static void
__OSTestThreadCancelNoLock()
{
   auto thread = OSGetCurrentThread();

   if (thread->cancelState) {
      if (thread->requestFlag == OSThreadRequest::Suspend) {
         __OSSuspendThreadNoLock(thread);
         __OSRescheduleNoLock();
      } else if (thread->requestFlag == OSThreadRequest::Cancel) {
         OSUnlockScheduler();
         OSExitThread(-1);
      }
   }
}

static void
__OSClearThreadStack32(OSThread *thread, uint32_t value)
{
   uint32_t clearStart = 0, clearEnd = 0;

   if (OSGetCurrentThread() == thread) {
      clearStart = thread->stackEnd + 4;
      clearEnd = OSGetStackPointer();
   } else {
      clearStart = thread->stackEnd + 4;
      clearEnd = thread->fiber->state.gpr[1];
   }

   for (auto addr = clearStart; addr < clearEnd; addr += 4) {
      *reinterpret_cast<uint32_t*>(gMemory.translate(addr)) = value;
   }
}

void
OSCancelThread(OSThread *thread)
{
   bool reschedule = false;

   OSLockScheduler();

   if (thread->requestFlag == OSThreadRequest::Suspend) {
      __OSWakeupThreadWaitForSuspensionNoLock(&thread->suspendQueue, -1);
      reschedule = true;
   }

   if (thread->suspendCounter != 0) {
      if (thread->cancelState == 0) {
         __OSResumeThreadNoLock(thread, thread->suspendCounter);
         reschedule = true;
      }
   }

   if (reschedule) {
      __OSRescheduleNoLock();
   }

   thread->suspendCounter = 0;
   thread->needSuspend = 0;
   thread->requestFlag = OSThreadRequest::Cancel;
   OSUnlockScheduler();

   if (OSGetCurrentThread() == thread) {
      OSExitThread(-1);
   }
}

long
OSCheckActiveThreads()
{
   // TODO: Count threads in active thread queue
   assert(false);
   return 1;
}

int32_t
OSCheckThreadStackUsage(OSThread *thread)
{
   uint32_t addr, result;
   OSLockScheduler();

   for (addr = thread->stackEnd + 4; addr < thread->stackStart; addr += 4) {
      auto val = *reinterpret_cast<uint32_t*>(gMemory.translate(addr));

      if (val != 0xfefefefe) {
         break;
      }
   }

   result = thread->stackStart - addr;
   OSUnlockScheduler();
   return result;
}

void
OSClearThreadStackUsage(OSThread *thread)
{
   OSLockScheduler();
   thread->attr &= ~OSThreadAttributes::StackUsage;
   OSUnlockScheduler();
}

void
OSContinueThread(OSThread *thread)
{
   OSLockScheduler();
   __OSResumeThreadNoLock(thread, thread->suspendCounter);
   __OSRescheduleNoLock();
   OSUnlockScheduler();
}

BOOL
OSCreateThread(OSThread *thread, ThreadEntryPoint entry, uint32_t argc, void *argv, uint8_t *stack, uint32_t stackSize, uint32_t priority, OSThreadAttributes::Flags attributes)
{
   // Create new fiber
   auto fiber = gProcessor.createFiber();

   // Setup OSThread
   memset(thread, 0, sizeof(OSThread));
   thread->entryPoint = entry;
   thread->userStackPointer = stack;
   thread->stackStart = gMemory.untranslate(stack);
   thread->stackEnd = thread->stackStart - stackSize;
   thread->basePriority = priority;
   thread->attr = attributes;
   thread->fiber = fiber;
   thread->state = OSThreadState::Ready;
   thread->id = gThreadId++;
   thread->suspendCounter = 1;
   fiber->thread = thread;

   // Write magic stack ending!
   gMemory.write(thread->stackEnd, 0xDEADBABE);

   // Setup ThreadState
   auto state = &fiber->state;
   memset(state, 0, sizeof(ThreadState));
   state->cia = thread->entryPoint;
   state->nia = state->cia + 4;
   state->gpr[1] = thread->stackStart;
   state->gpr[3] = argc;
   state->gpr[4] = gMemory.untranslate(argv);

   // Initialise tracer
   traceInit(state, 128);

   return TRUE;
}

void
OSDetachThread(OSThread *thread)
{
   OSLockScheduler();
   thread->attr |= OSThreadAttributes::Detached;
   OSUnlockScheduler();
}

void
OSExitThread(int value)
{
   auto thread = OSGetCurrentThread();
   OSLockScheduler();
   thread->exitValue = value;

   if (thread->attr & OSThreadAttributes::Detached) {
      thread->state = OSThreadState::None;
   } else {
      thread->state = OSThreadState::Moribund;
   }

   __OSWakeupThreadNoLock(&thread->joinQueue);
   __OSWakeupThreadWaitForSuspensionNoLock(&thread->suspendQueue, -1);
   OSUnlockScheduler();
   gProcessor.exit();
}

void
OSGetActiveThreadLink(OSThread *thread, OSThreadLink *link)
{
   link->next = thread->activeLink.next;
   link->prev = thread->activeLink.prev;
}

OSThread *
OSGetCurrentThread()
{
   return gProcessor.getCurrentFiber()->thread;
}

OSThread *
OSGetDefaultThread(uint32_t coreID)
{
   if (coreID >= 3) {
      return nullptr;
   }

   return gDefaultThreads[coreID];
}

uint32_t
OSGetStackPointer()
{
   return OSGetCurrentThread()->fiber->state.gpr[1];
}

uint32_t
OSGetThreadAffinity(OSThread *thread)
{
   return thread->attr & OSThreadAttributes::AffinityAny;
}

const char *
OSGetThreadName(OSThread *thread)
{
   return thread->name;
}

uint32_t
OSGetThreadPriority(OSThread *thread)
{
   return thread->basePriority;
}

uint32_t
OSGetThreadSpecific(uint32_t id)
{
   return OSGetCurrentThread()->specific[id];
}

void
OSInitThreadQueue(OSThreadQueue *queue)
{
   OSInitThreadQueueEx(queue, nullptr);
}

void
OSInitThreadQueueEx(OSThreadQueue *queue, void *parent)
{
   queue->head = nullptr;
   queue->tail = nullptr;
   queue->parent = parent;
}

void
OSInsertThreadQueue(OSThreadQueue *queue, OSThread *thread)
{
   thread->queue = queue;

   if (!queue->head) {
      thread->link.prev = nullptr;
      thread->link.next = nullptr;
      queue->head = thread;
      queue->tail = thread;
   } else {
      queue->tail->link.next = thread;
      thread->link.prev = queue->tail;
      thread->link.next = nullptr;
      queue->tail = thread;
   }
}

BOOL
OSIsThreadSuspended(OSThread *thread)
{
   return thread->suspendCounter > 0;
}

BOOL
OSIsThreadTerminated(OSThread *thread)
{
   return thread->state == OSThreadState::None
      || thread->state == OSThreadState::Moribund;
}

BOOL
OSJoinThread(OSThread *thread, be_val<int> *val)
{
   OSLockScheduler();

   if (thread->attr & OSThreadAttributes::Detached) {
      OSUnlockScheduler();
      return FALSE;
   }

   if (thread->state != OSThreadState::Moribund) {
      __OSSleepThreadNoLock(&thread->joinQueue);
      __OSRescheduleNoLock();
   }
   
   if (val) {
      *val = thread->exitValue;
   }

   OSUnlockScheduler();
   return TRUE;
}

void
OSPrintCurrentThreadState()
{
   // TODO: Implement OSPrintCurrentThreadState
   assert(false);
}

int32_t
OSResumeThread(OSThread *thread)
{
   OSLockScheduler();
   auto old = __OSResumeThreadNoLock(thread, 1);
   
   if (thread->suspendCounter == 0) {
      __OSRescheduleNoLock();
   }

   OSUnlockScheduler();
   return old;
}

BOOL
OSRunThread(OSThread *thread, ThreadEntryPoint entry, uint32_t argc, p32<void> argv)
{
   BOOL result = false;

   OSLockScheduler();

   if (OSIsThreadTerminated(thread)) {
      // Setup state
      auto state = &thread->fiber->state;
      state->cia = thread->entryPoint;
      state->nia = state->cia + 4;
      state->gpr[1] = thread->stackStart;
      state->gpr[3] = argc;
      state->gpr[4] = static_cast<uint32_t>(argv);

      // Setup thread
      thread->state = OSThreadState::Ready;
      thread->suspendCounter = 1;
      thread->requestFlag = OSThreadRequest::None;
      thread->needSuspend = 0;

      __OSResumeThreadNoLock(thread, 1);
      __OSRescheduleNoLock();
   }

   OSUnlockScheduler();
   return result;
}

OSThread *
OSSetDefaultThread(uint32_t core, OSThread *thread)
{
   auto old = gDefaultThreads[core];
   gDefaultThreads[core] = thread;
   return old;
}

BOOL
OSSetThreadAffinity(OSThread *thread, uint32_t affinity)
{
   OSLockScheduler();
   thread->attr &= ~OSThreadAttributes::AffinityAny;
   thread->attr |= affinity;
   __OSRescheduleNoLock();
   OSUnlockScheduler();
   return TRUE;
}

BOOL
OSSetThreadCancelState(BOOL state)
{
   auto thread = OSGetCurrentThread();
   auto old = thread->cancelState;
   thread->cancelState = state;
   return old;
}

void
OSSetThreadName(OSThread* thread, const char *name)
{
   thread->name = name;
}

BOOL
OSSetThreadPriority(OSThread* thread, uint32_t priority)
{
   if (priority > 31) {
      return FALSE;
   }

   OSLockScheduler();
   thread->basePriority = priority;
   __OSRescheduleNoLock();
   OSUnlockScheduler();
   return TRUE;
}

BOOL
OSSetThreadRunQuantum(OSThread* thread, uint32_t quantum)
{
   // TODO: Implement OSSetThreadRunQuantum
   assert(false);
   return FALSE;
}

void
OSSetThreadSpecific(uint32_t id, uint32_t value)
{
   OSGetCurrentThread()->specific[id] = value;
}


BOOL
OSSetThreadStackUsage(OSThread *thread)
{
   OSLockScheduler();

   if (!thread) {
      thread = OSGetCurrentThread();
   } else if (thread->state == OSThreadState::Running) {
      OSUnlockScheduler();
      return FALSE;
   }

   __OSClearThreadStack32(thread, 0xfefefefe);
   thread->attr |= OSThreadAttributes::StackUsage;
   OSUnlockScheduler();
   return TRUE;
}

void
OSSleepThread(OSThreadQueue *queue)
{
   OSLockScheduler();
   __OSSleepThreadNoLock(queue);
   __OSRescheduleNoLock();
   OSUnlockScheduler();
}

void
OSSleepTicks(Time ticks)
{
   OSLockScheduler();
   // Create alarm
   // Set alarm interval
   __OSSleepThreadNoLock(nullptr);
   __OSRescheduleNoLock();
   OSUnlockScheduler();
}

uint32_t
OSSuspendThread(OSThread *thread)
{
   OSLockScheduler();
   int32_t result;

   if (thread->state == OSThreadState::Moribund || thread->state == OSThreadState::None) {
      result = -1;
      goto exit;
   }

   if (thread->requestFlag == OSThreadRequest::Cancel) {
      result = -1;
      goto exit;
   }

   auto curThread = OSGetCurrentThread();

   if (curThread == thread) {
      if (thread->cancelState) {
         result = -1;
         goto exit;
      }

      thread->needSuspend++;
      result = thread->suspendCounter;
      __OSSuspendThreadNoLock(thread);
      __OSRescheduleNoLock();
      goto exit;
   } else {
      if (thread->suspendCounter != 0) {
         result = thread->suspendCounter++;
         goto exit;
      } else {
         thread->needSuspend++;
         thread->requestFlag = OSThreadRequest::Suspend;
         __OSSleepThreadNoLock(&thread->suspendQueue);
         __OSRescheduleNoLock();
         result = thread->suspendResult;
      }
   }

exit:
   OSUnlockScheduler();
   return result;
}

void
OSTestThreadCancel()
{
   OSLockScheduler();
   __OSTestThreadCancelNoLock();
   OSUnlockScheduler();
}

void
OSWakeupThread(OSThreadQueue *queue)
{
   OSLockScheduler();
   __OSWakeupThreadNoLock(queue);
   OSUnlockScheduler();
}

void
OSYieldThread()
{
   gProcessor.yield();
}

void
CoreInit::registerThreadFunctions()
{
   RegisterKernelFunction(OSCancelThread);
   RegisterKernelFunction(OSCheckActiveThreads);
   RegisterKernelFunction(OSCheckThreadStackUsage);
   RegisterKernelFunction(OSClearThreadStackUsage);
   RegisterKernelFunction(OSContinueThread);
   RegisterKernelFunction(OSCreateThread);
   RegisterKernelFunction(OSDetachThread);
   RegisterKernelFunction(OSExitThread);
   RegisterKernelFunction(OSGetActiveThreadLink);
   RegisterKernelFunction(OSGetCurrentThread);
   RegisterKernelFunction(OSGetDefaultThread);
   RegisterKernelFunction(OSGetStackPointer);
   RegisterKernelFunction(OSGetThreadAffinity);
   RegisterKernelFunction(OSGetThreadName);
   RegisterKernelFunction(OSGetThreadPriority);
   RegisterKernelFunction(OSGetThreadSpecific);
   RegisterKernelFunction(OSInitThreadQueue);
   RegisterKernelFunction(OSInitThreadQueueEx);
   RegisterKernelFunction(OSInsertThreadQueue);
   RegisterKernelFunction(OSIsThreadSuspended);
   RegisterKernelFunction(OSIsThreadTerminated);
   RegisterKernelFunction(OSJoinThread);
   RegisterKernelFunction(OSPrintCurrentThreadState);
   RegisterKernelFunction(OSResumeThread);
   RegisterKernelFunction(OSRunThread);
   RegisterKernelFunction(OSSetThreadAffinity);
   RegisterKernelFunction(OSSetThreadCancelState);
   RegisterKernelFunction(OSSetThreadName);
   RegisterKernelFunction(OSSetThreadPriority);
   RegisterKernelFunction(OSSetThreadRunQuantum);
   RegisterKernelFunction(OSSetThreadSpecific);
   RegisterKernelFunction(OSSetThreadStackUsage);
   RegisterKernelFunction(OSSleepThread);
   RegisterKernelFunction(OSSleepTicks);
   RegisterKernelFunction(OSSuspendThread);
   RegisterKernelFunction(OSTestThreadCancel);
   RegisterKernelFunction(OSWakeupThread);
   RegisterKernelFunction(OSYieldThread);
}
