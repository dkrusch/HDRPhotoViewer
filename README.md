# HDRPhotoViewer

HDRPhotoViewer is a Windows desktop application that renders SDR images as textures in full screen with DirectX 12, the intent is for this to be used in conjuction with the NVIDIA RTX HDR overlay to view those images in HDR. To setup NVIDIA RTX HDR follow this guide (not mine): https://docs.google.com/document/d/1OIVKk8njrDTELsIZUrTBod_LdPB1sz9FieK6h1DfzF0/edit?tab=t.0

To install click the <> Code button and download as zip, once you've unzipped you can try and launch the exe, if you get any errors on you should the Setup.cmd as administrator, this will install Microsoft Visual C++ Redist. Once you've run the app for the first time you will need to go into your NVIDIA app under Graphics and add a program, you can just add the HDRViewer.exe. Once you do that you should only need to relaunch once to see your photos in HDR. It's important to note NVIDIA HDR doesn't work with multiple monitor setups.

## Keyboard Commands

- **Left/Right Arrows**: Move to previous/next image in the list.
- **Up/Down Arrows**: Zoom in/out.
- **R**: Reset zoom and pan.
- **T**: Cycle through different sorting modes (Name, Modified Date, Created Date).
- **O**: Open a new image file.
- **I**: Toggle drawing of image information (file name, dimensions, HDR format).

## Mouse Commands

- **Left Click**: Move to previous image in the list.
- **Right Click**: Move to next image in the list.
- **Mouse Wheel**: Zoom in/out.
- **Middle Click and Drag**: Pan image.

## Notes

- The program uses the [stb_image.h](https://github.com/nothings/stb) library to load images.
- The program uses the [DirectXTex](https://github.com/Microsoft/DirectXTex) library to convert images to HDR format.
- The program uses the [Direct3D 11](https://docs.microsoft.com/en-us/windows/desktop/direct3d11/direct3d-11-graphics) API to render the images.
- The program uses the [Windows API](https://docs.microsoft.com/en-us/windows/desktop/apiindex/windows-api-index) to create the window and handle events.
