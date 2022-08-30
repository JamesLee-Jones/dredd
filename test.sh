#!/bin/bash

rm -f out*.txt

if 
  !(clang -pedantic -Wall -O0 -c csmith-out.c  >out.txt 2>&1 &&\
  ! grep 'conversions than data arguments' out.txt &&\
  ! grep 'incompatible redeclaration' out.txt &&\
  ! grep 'ordered comparison between pointer' out.txt &&\
  ! grep 'eliding middle term' out.txt &&\
  ! grep 'end of non-void function' out.txt &&\
  ! grep 'invalid in C99' out.txt &&\
  ! grep 'specifies type' out.txt &&\
  ! grep 'should return a value' out.txt &&\
  ! grep 'uninitialized' out.txt &&\
  ! grep 'incompatible pointer to' out.txt &&\
  ! grep 'incompatible integer to' out.txt &&\
  ! grep 'type specifier missing' out.txt)
then
  exit 1
fi

echo "Here"
# Check the program yields a different binary when compiled with and 
# without the mutant.
./llvm-project/build/bin/clang -O3 csmith-out.c -o bin1
DREDD_ENABLED_MUTATION=$1 ./llvm-project/build/bin/clang -O3 csmith-out.c -o bin2 # This should use the mutated version of clang
if 
 diff bin1 bin2
then 
  exit 1
fi

echo "Different"
# Check the program runs without error after compiling with asan and ubsan.
clang -O3 -fsanitize=address csmith-out.c
clang -O3 -fsanitize-recover=undefined csmith-out.c
./a.out
rm a.out

# Check 1 and 2 yield different results when executed.
if
  diff <(./bin1) <(./bin2)
then
  exit 1
fi

exit 0
