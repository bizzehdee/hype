echo HYPE-PROBE-BEGIN
echo forcing a full driver connect first: UEFI binds lazily
connect -r
echo HYPE-PROBE-MAP
map
echo HYPE-PROBE-BLK0
dblk BLK0 0 1
echo HYPE-PROBE-BLK1
dblk BLK1 0 1
echo HYPE-PROBE-BLK2
dblk BLK2 0 1
echo HYPE-PROBE-BLK3
dblk BLK3 0 1
echo HYPE-PROBE-BLK4
dblk BLK4 0 1
echo HYPE-PROBE-END
