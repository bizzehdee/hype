# Boot 18 -- the hitching was my diagnostic

Boot 17 got key repeat working, and then everything else got worse. The hitching was my
fault, directly and measurably.

## DO NOT UNPLUG THE HYPE DRIVE

`HYPEBOOT` is the boot medium **and** the log medium.

## What boot 17 showed

Your log measured the freezes:

```
#764 SLOW POLL slot=4 ep=0x81 took 26 ms
#764 SLOW POLL slot=1 ep=0x81 took 45 ms
#764 SLOW POLL slot=6 ep=0x81 took 45 ms
```

Those are the keyboard poll blocking for 26 and 45 milliseconds. What was it doing? Printing
a **nine-line diagnostic**, from inside the poll. The thing I added to explain the hitching
was causing it.

Worse, it was reporting a fault that was not there. It compared the controller's dequeue
pointer against hype's -- and that field is only valid to read when an endpoint is STOPPED
(xHCI 4.12.2). While the endpoint is running the controller keeps it internally and does not
write it back, so hype was reading a stale value left over from setup and calling the
difference a failure. Every one it reported on your machine was a healthy endpoint: two in
boot 16 on a keyboard that went on working for another 1,326 reports.

QEMU updates that field during normal running, which is why the check looked sound on my
desk and was nonsense on yours. It is now on the list of real QEMU-vs-hardware differences
in `QEMU-VS-HARDWARE.md`.

The whole check is gone.

## On "repeat works, really slow"

That was the correct default. A PS/2 keyboard powers on at 500 ms delay and about 11 repeats
a second, and until the guest asks for something else that is what it should do -- it is what
a real keyboard plugged into a real machine does.

Your guest DID ask for faster. This is in the log:

```
host-hid: guest set typematic 0x00 -> delay 250ms rate period 33ms
```

250 ms and 30 a second -- the fastest setting there is. It arrived late in the run though,
right as the keyboard went, so you probably never felt it. This boot should feel different
once the guest has settled.

## What to do

Same as last time. No hot-plugging.

1. Boot, wait for the guest login prompt.
2. Type fast. Do keys still go missing?
3. Hold a letter, then backspace, then an arrow key. Each should repeat after a short delay
   and run at a steady rate.
4. **Say whether the hitching is gone.** That is the main question this boot answers.
5. A few minutes, then power off.

## Being straight about what is not proven

I know why the hitching happened and it is fixed. I do **not** know for certain why the
keyboard stopped entirely after your first long key press. The 45 ms stalls are the obvious
suspect and they are gone, but I have not reproduced that failure, so I cannot promise it.

If the keyboard dies again, note what you were doing and stop -- the log will be cleaner now
that it is not full of a diagnostic that was lying.

## Afterwards

    cp \HYPE.LOG \RUN1A.LOG  tools/hw-val-2026-08-25/logs/boot-18/
