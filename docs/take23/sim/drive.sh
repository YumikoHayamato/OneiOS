D=/tmp/claude-1000/t23/shtest/fake/d
sleep 30 & pid=$!
. ./orch-sim.sh
sleep 1.2; echo "cap after idle start: $(cat /tmp/claude-1000/t23/shtest/fake/gmax) clamp $(cat /tmp/claude-1000/t23/shtest/fake/uclamp)"
echo "60 %" > /tmp/claude-1000/t23/shtest/fake/kgsl/gpu_busy_percentage; sleep 0.6; echo "after 60% x2 looks: $(cat /tmp/claude-1000/t23/shtest/fake/gmax)"
echo "80 %" > /tmp/claude-1000/t23/shtest/fake/kgsl/gpu_busy_percentage; sleep 0.6; echo "after 80%: $(cat /tmp/claude-1000/t23/shtest/fake/gmax)"
echo "10 %" > /tmp/claude-1000/t23/shtest/fake/kgsl/gpu_busy_percentage; sleep 4.6; echo "after 4.6 s at 10%: $(cat /tmp/claude-1000/t23/shtest/fake/gmax)"
echo "oneios-display-pace: 10 s: swaps +789, 103.6 a second while moving; apart by <1 refresh +15, 1 +696, 2 (one skipped) +19, more +16, pauses +43; refused +0" >> $D/serial.txt
sleep 5.5; echo "after a good round: clamp $(cat /tmp/claude-1000/t23/shtest/fake/uclamp)"
echo "oneios-display-pace: 10 s: swaps +458, 57.9 a second while moving; apart by <1 refresh +52, 1 +64, 2 (one skipped) +198, more +114, pauses +30; refused +0" >> $D/serial.txt
sleep 5.5; echo "after a bad round: clamp $(cat /tmp/claude-1000/t23/shtest/fake/uclamp) cap $(cat /tmp/claude-1000/t23/shtest/fake/gmax)"
echo 99000 > /tmp/claude-1000/t23/shtest/fake/thermal/thermal_zone1/temp; sleep 2.5; echo "hot: clamp $(cat /tmp/claude-1000/t23/shtest/fake/uclamp) cap $(cat /tmp/claude-1000/t23/shtest/fake/gmax)"
kill $pid; wait $pid 2>/dev/null
orch_end
echo "end: cap $(cat /tmp/claude-1000/t23/shtest/fake/gmax) clamp file: $(ls /tmp/claude-1000/t23/shtest/fake/uclamp 2>&1 | tail -c 30)"
cat $D/orch.txt
