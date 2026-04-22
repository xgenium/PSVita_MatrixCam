# MatrixCam PS Vita Version
This is a [MatrixCam](https://github.com/xgenium/MatrixCam) PS Vita version with some additional functionality (i.e. 2 cameras)

## Controls
* **Cross:** Toggle between the front and rear cameras
* **Triangle:** Toggle between modes (Normal and Matrix)

# Build
Ensure that you have the [VitaSDK](https://vitasdk.org/) installed and configured
Then you can make the VPK file:
```bash
mkdir build
cd build
cmake ..
cmake --build .
```
Then transfer VPK and font.ttf anywhere on your Vita via USB or FTP using VitaShell.

# Install & Run
Once transferred everything, install the VPK, then copy font.ttf to ux0:app/THIS_APP_ID (you can find it by sorting by date). Now you can run the program

# Additional info
More functionality (e.g. changing contrast, brightness) will be added soon.
I believe that this program can be much faster (maybe by using GPU). I will play with the code and try to optimize it.
