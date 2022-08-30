set -e

if [ $# -ne 1 ]
then
  echo "Please specify the mutant to enable."
  exit 1
fi

while true
do
  csmith > out-csmith.c
  if 
    ./test.sh "$1"
  then
    break
  fi
done

# Should probably pass $1 to test.sh
creduce ./test.sh out-csmith.c

