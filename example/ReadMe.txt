TextureLoader.asi - GTA Vice City PC v1.0
=========================================

Runtime PNG-to-TXD texture replacer for Grand Theft Auto: Vice City (PC v1.0, 32-bit x86).
Replaces textures from PNGs using TextureLoader.ini across loose .txd files and
streamed .img / modloader\ TXDs in memory without modifying base game files on disk.

IMPORTANT PREREQUISITES
---------------------
1. ASI LOADER REQUIRED:
   GTA Vice City does not load .asi plugins on its own. You MUST have a 32-bit x86
   ASI loader installed (such as Ultimate ASI Loader / dinput8.dll) in your game folder.

2. GTA VICE CITY v1.0:
   Hooks and offsets target GTA Vice City PC v1.0.

WHERE TO PUT THE FILES
----------------------
- Place TextureLoader.asi and TextureLoader.ini in your GTA Vice City root folder
  (next to gta-vc.exe), inside a scripts\ folder, OR inside a modloader\ subfolder
  (for example: modloader\TextureLoader\TextureLoader.asi + TextureLoader.ini).
- Keep TextureLoader.ini in the SAME folder as TextureLoader.asi.
- Modded TXDs, .img archives, or PNG texture packs can live in modloader\ subfolders
  or custom folders referenced by TextureLoader.ini (relative paths resolve from
  the folder containing TextureLoader.asi).

HOW TO USE THE WHITE DOT CROSSHAIR EXAMPLE
----------------------------------------
1. Copy TextureLoader.ini (from TextureLoader.ini) into the same folder as
   TextureLoader.asi (game root, scripts\, or your modloader\ subfolder).
2. Create textures\hud\ next to TextureLoader.asi and place siteM16.png there:
   \textures\hud\siteM16.png
3. Launch gta-vc.exe and aim with a weapon to test the white dot crosshair.

TO ADD YOUR OWN TEXTURE REPLACEMENTS
----------------------------------
1. Under [Settings], keep Enable = 1.
2. Add a section matching the target TXD stem (without .txd), for example [hud] or [mesa].
3. Set Enable = 1, set Folder = <path>\, and map:
   <exact_texture_name_in_txd> = <file.png>
   (Or place <texture_name>.png inside Folder to match by texture name automatically.)

TROUBLESHOOTING
---------------
- Check TextureLoader.log in the same folder as TextureLoader.asi after launching.
- If no log is generated: verify your 32-bit ASI loader is installed and gta-vc.exe is v1.0.
- If a texture does not swap: confirm the INI key matches the texture name inside the
  target .txd and verify that the .png path exists.