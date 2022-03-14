# OBS Asynchronous Source Duplication Plugin

## Introduction

This plugin provides a duplicated source of an asynchronous source.

Asynchronous filters cannot be applied to a scene or a group.
That means you cannot have one source as two scene-items with and without the asynchronous filter.

For example, you want to have two sources;
one is a video camera input and the other is the same video camera input but having a `Video Delay (Async)` filter to delay.
However, you cannot have two separated sources that access the same video capture device.

In such case, this plugin help you to duplicate the asynchronous source.

## Usage

1. Add `Asynchronous Source Duplication Filter` to your source
   1. Open filter dialog for the source you want to duplicate.
   2. Click `+` at the bottom of `Audio/Video Filters` list.
   3. Click `Asynchronous Source Duplication Filter`.
2. Add `Asynchronous Source Duplicator` to your scene
   1. Select the scene you want to have the duplicated source.
   2. Click `+` at the bottom of `Sources` list.
   3. Click `Asynchronous Source Duplicator`.
   4. Type the name of the source that you want to duplicate.

## Properties

### Source Name

Choose the source you want to duplicate.

## Build and install
### Linux
Use cmake to build on Linux. After checkout, run these commands.
```
sed -i 's;${CMAKE_INSTALL_FULL_LIBDIR};/usr/lib;' CMakeLists.txt
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr ..
make
sudo make install
```

### macOS
Use cmake to build on Linux. After checkout, run these commands.
```
mkdir build && cd build
cmake ..
make
```
