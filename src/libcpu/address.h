#pragma once
#include <cstdint>
#include <common/align.h>

namespace cpu
{

template<class Type>
class Address
{
public:
   using StorageType = uint32_t;

   constexpr Address() = default;

   constexpr explicit Address(StorageType address) :
      mAddress(address)
   {
   }

   explicit operator bool() const
   {
      return !!mAddress;
   }

   constexpr bool operator == (const Address &other) const
   {
      return mAddress == other.mAddress;
   }

   constexpr bool operator != (const Address &other) const
   {
      return mAddress != other.mAddress;
   }

   constexpr bool operator >= (const Address &other) const
   {
      return mAddress >= other.mAddress;
   }

   constexpr bool operator <= (const Address &other) const
   {
      return mAddress <= other.mAddress;
   }

   constexpr bool operator > (const Address &other) const
   {
      return mAddress > other.mAddress;
   }

   constexpr bool operator < (const Address &other) const
   {
      return mAddress < other.mAddress;
   }

   constexpr Address &operator += (ptrdiff_t value)
   {
      mAddress = static_cast<StorageType>(mAddress + value);
      return *this;
   }

   constexpr Address &operator -= (ptrdiff_t value)
   {
      mAddress = static_cast<StorageType>(mAddress - value);
      return *this;
   }

   constexpr Address operator + (ptrdiff_t value) const
   {
      return Address { static_cast<StorageType>(mAddress + value) };
   }

   constexpr Address operator - (ptrdiff_t value) const
   {
      return Address { static_cast<StorageType>(mAddress - value) };
   }

   constexpr ptrdiff_t operator - (const Address &other) const
   {
      return mAddress - other.mAddress;
   }

   constexpr StorageType getAddress() const
   {
      return mAddress;
   }

private:
   StorageType mAddress = 0;
};

template<typename Type>
struct AddressRange
{
   using address_type = Address<Type>;

   address_type start;
   uint32_t size;
};

class Physical;
class Virtual;

using PhysicalAddress = Address<Physical>;
using VirtualAddress = Address<Virtual>;

using PhysicalAddressRange = AddressRange<Physical>;
using VirtualAddressRange = AddressRange<Virtual>;

} // namespace cpu

template<typename Type>
constexpr inline cpu::Address<Type>
align_up(cpu::Address<Type> value, size_t alignment)
{
   return cpu::Address<Type> { align_up(value.getAddress(), alignment) };
}

template<typename Type>
constexpr inline cpu::Address<Type>
align_down(cpu::Address<Type> value, size_t alignment)
{
   return cpu::Address<Type> { align_down(value.getAddress(), alignment) };
}
