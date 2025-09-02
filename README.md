# HDRPhotoViewer

A simple photo viewer that uses the dx12 engine to render images as textures for nvidia hdr overlay to hook onto. Provides some basic commands to navigate and zoom in/out.

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
