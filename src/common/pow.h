#pragma once

template<typename T>
inline T Log2(T x) {
   T y = 0;

   while (x > 1) {
      x >>= 1;
      y++;
   }

   return y;
}