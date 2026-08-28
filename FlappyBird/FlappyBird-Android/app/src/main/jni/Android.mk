# El codigo del juego es el MISMO que en Windows y Linux: vive en FlappyBird/src
# y aqui solo se referencia. No hay copia ni fork.
LOCAL_PATH := $(call my-dir)
GAME_SRC   := $(LOCAL_PATH)/../../../../../src
SQLITE_SRC := $(LOCAL_PATH)/../../../../../../third_party/sqlite

include $(CLEAR_VARS)
LOCAL_MODULE    := sqlite3
LOCAL_SRC_FILES := $(SQLITE_SRC)/sqlite3.c
LOCAL_CFLAGS    := -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=0 -O2
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE    := flappybird
LOCAL_SRC_FILES := $(GAME_SRC)/main.cpp $(GAME_SRC)/scoredb.cpp
LOCAL_C_INCLUDES := $(GAME_SRC) $(SQLITE_SRC)
LOCAL_CPPFLAGS  := -std=c++17 -fexceptions -frtti -Wno-unknown-pragmas
LOCAL_STATIC_LIBRARIES := sqlite3
# Estas tres se listan aunque el juego no las use directamente. SFMLActivity.cpp
# hace dlopen de una lista FIJA al arrancar y llama a exit(1) si falta cualquiera:
#   openal, sfml-system, sfml-window, sfml-graphics, sfml-audio, sfml-network
#  - sfml-activity la nombra el AndroidManifest (android.app.lib_name) y no la
#    enlaza nadie, asi que sin declararla ndk-build no la empaqueta.
#  - sfml-network no se usa en el juego, pero la activity la exige igual.
LOCAL_SHARED_LIBRARIES := sfml-system sfml-window sfml-graphics sfml-audio \
                          sfml-network sfml-activity openal
LOCAL_WHOLE_STATIC_LIBRARIES := sfml-main
LOCAL_LDLIBS    := -landroid -llog
include $(BUILD_SHARED_LIBRARY)

$(call import-module,third_party/sfml)
