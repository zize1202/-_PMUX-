yosys -ql run.log run.ys
if grep "SAT temporal induction proof finished - model found for base case: FAIL!" run.log > /dev/null ; then
	echo FAIL > status
	echo "Could not prove equivalence of partition 'test3.gpioout' using strategy 'induction': partitions not equivalent"
elif grep "Reached maximum number of time steps -> proof failed." run.log > /dev/null ; then
	echo UNKNOWN > status
	echo "Could not prove equivalence of partition 'test3.gpioout' using strategy 'induction': equivalence unknown"
elif grep "Interrupted SAT solver: TIMEOUT!" run.log > /dev/null ; then
	echo UNKNOWN > status
	echo "Could not prove equivalence of partition 'test3.gpioout' using strategy 'induction': timeout"
elif grep "Induction step proven: SUCCESS!" run.log > /dev/null ; then
	echo PASS > status
	echo "Proved equivalence of partition 'test3.gpioout' using strategy 'induction'"
else
	echo ERROR > status
	echo "Execution of strategy 'induction' on partition 'test3.gpioout' encountered an error.
Details can be found in 'rb/strategies/test3.gpioout/induction/run.log'."
	exit 1
fi
exit 0

