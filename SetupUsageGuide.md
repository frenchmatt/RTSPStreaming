# Setup and Usage Guide
## A How-To Guide to Setting Up and Using a New Unreal Project for RTSPStreaming

#### Add the Plugin

This plugin can be added to any project. Here's how to do it. See the FAQs if there are any issues.

1. Start with the project closed. Download the repository and extract it into ...\Unreal Projects\Project\Plugins. (You may have to create the Plugins folder if it does not already exist).
2. Try to open the project and rebuild the plugin. Hopefully it builds, but if not, see FAQs.

#### Test the Stream

Everything should be set up at this point.

1. Package the game, and run it with the following command and flags (change the IP to your own and pick a port). You can also remove the first four flags, [-ResX=1280 -ResY=720 -ForceRes -RenderOffScreen], and headless mode will be turned off:

    ```
    <Project>.exe -ResX=1280 -ResY=720 -ForceRes -RenderOffScreen -RTSPStreamingIP=172.24.50.106 -RTSPStreamingPort=8554
    ```

2. Open the stream with the MRL. Here is the command I use to get a low latency UDP stream (Around 30 ms latency streaming to localhost):

    ```
    ffplay -an -hide_banner -showmode 0 -fast -sync ext -vcodec h264 -framedrop -infbuf -probesize 32 -flags low_delay -me_method zero -flags2 fast -avioflags direct -fflags discardcorrupt -fflags nobuffer -flush_packets 1 -preset ultrafast -profile baseline -tune zerolatency -i rtsp://127.0.0.1:8554/stream/1
    ```

    You can opt for RTSP over TCP by using the following command: (Note the added -rtsp_transport tcp flag)

     ```
    ffplay -an -hide_banner -showmode 0 -fast -sync ext -vcodec h264 -framedrop -infbuf -probesize 32 -flags low_delay -me_method zero -flags2 fast -avioflags direct -fflags discardcorrupt -fflags nobuffer -flush_packets 1 -preset ultrafast -profile baseline -tune zerolatency -rtsp_transport tcp -i rtsp://127.0.0.1:8554/stream/1
    ```
    
    You can also opt to disable the streamer entirely. GeForce GPUs have a set limit of two encoding sessions per, so it may be necessary to choose which instances should be streaming in a multiplayer setup. Disabling the streamer won't use one of those two slots. Use the following command: (Note the added -DisableRTSPStreaming=true) 
    
    ```
    <Project>.exe -ResX=1280 -ResY=720 -ForceRes -RenderOffScreen -DisableRTSPStreaming=true
    ```

#### FAQs

1. I cannot build the Plugin. If this happens, you can debug by Right-Clicking the .uproject file and Generating Visual Studio Project Files. Then open the .sln. I built this in 4.22.3 if that helps. Also, check out the .Build.cs file, because some directories are hard-coded like the D3D11RHI dir.

2. My project is new and empty, or the stream is black. A default camera viewport needs to be added to the level. You can do this by adding a camera actor into the level and setting the default viewport in the Level Blueprint editor. Use the 'Set View Target with Blend' node executed on 'Event BeginPlay'. Right click to 'Add Reference to CameraActor' for the camera actor you added, and wire that to 'New View Target' on the 'Set View Target with Blend' node. Finally, add a 'Get Player Controller' and wire the return value to 'Target' (see Blueprint below). Save, compile and close the Blueprint Editor. Make sure that your level is saved and made the 'Game Default Map' Under Edit -> Project Settings -> Maps & Modes.