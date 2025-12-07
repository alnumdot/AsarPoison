# AsarPoison
C++ Program to poison Electron Apps without unpacking them\
\
This inserts a specific line into the main.js file of the .asar directory\
that main.js file acts like an entry point of the electron app, so anything executed in there gets executed every start\
\
this is just a neat idea i had and shouldn't be used in a malicious way!\
\
the whole code is a bit messy, sorry for that.

This should work for any Electron app that uses asar files, if they don't then\
they usually expose the main.js in some folder (which is even easier to poison)\
\
Tested on\
**Discord**\
**Discord Canary**\
**Visual Studio Code**\
**Obsidian**\
**Postman**\
**Arduino IDE**\
~Element~ doesn't work anymore (i guess they implemented integrity checks)


# Discord PREVIEW
![Preview](https://raw.githubusercontent.com/alnumdot/AsarPoison/refs/heads/main/devenv_3s1yDgT5bS.gif)

# Obsidian PREVIEW
![Preview](https://raw.githubusercontent.com/alnumdot/AsarPoison/refs/heads/main/devenv_iBmoCPVg1o.gif)
