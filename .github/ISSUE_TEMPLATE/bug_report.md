---
name: Bug report
about: Something looks wrong, runs badly or will not start
---

**Mac and macOS** (e.g. MacBook Pro M3 Pro, macOS 27.0):

**MetalQuake version** (the date in the zip's name):

**Quality tier** (Options → M5 Quality):

**What happened, and what you expected:**

**If it crashed:** macOS saved a report. Open the **Console** app, choose *Crash Reports*,
select the newest `MetalQuake` entry, then *Edit → Select All* and *Copy*, and paste it
below inside a pair of ``` lines (GitHub will not take the `.ips` file itself as an
attachment; zip it first if you would rather attach it). It says where in the code the
crash happened.

**Map and where** (a screenshot helps; F12 saves one to
`~/Library/Application Support/darkplaces/m5/screenshots`):

**If it will not find the game files:** open the console from the error screen, type
`path`, and paste what it prints. That one command tells us where the app is really
running from.

**Frame rate, if it is a performance report:** type `cl_showfps 1` in the console.
