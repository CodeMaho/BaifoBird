# arm64-v8a cubre practicamente cualquier movil actual. Para anadir 32 bits hay
# que compilar tambien SFML para armeabi-v7a.
APP_ABI      := arm64-v8a
APP_PLATFORM := android-21
APP_STL      := c++_shared
APP_CPPFLAGS := -std=c++17 -fexceptions -frtti
APP_OPTIM    := release
