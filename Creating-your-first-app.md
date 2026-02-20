## Your First App

Creating an app is incredibly straightforward. You only need two files in a directory inside `/apps/` on the SD Card:

1. `app.json`: A simple file describing your app.

```
1     {
2         "name": "My Awesome App",
3         "description": "The best app ever.",
4         "version": "1.0"
5     }
```

2. `main.lua`: The entry point for your code, which runs in a simple loop.

```
 1     -- The 'picocalc' global is your gateway to the hardware
 2     local pc = picocalc
 3 
 4     -- Main app loop
 5     while true do
 6         -- Check for the ESC key to exit back to the launcher
 7         if pc.input.getButtonsPressed() & pc.input.BTN_ESC ~= 0 then
 8             return
 9         end
10 
11         -- Drawing
12         pc.display.clear(pc.display.BLACK)
13         pc.display.drawText(100, 150, "Hello, World!", pc.display.WHITE)
14         pc.display.flush() -- Push your changes to the screen
15 
16         -- Sleep to maintain a steady frame rate (~60 FPS)
17         pc.sys.sleep(16)
18     end
```