/**
 * Mupen64PlusAE, an N64 emulator for the Android platform
 *
 * Copyright (C) 2013 Paul Lamb
 *
 * This file is part of Mupen64PlusAE.
 *
 * Mupen64PlusAE is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Mupen64PlusAE is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Mupen64PlusAE. If
 * not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Paul Lamb, littleguy77
 */

#include <dlfcn.h>
#include <SDL.h>
#include "ae_bridge.h"

#define M64P_CORE_PROTOTYPES
#include "m64p_frontend.h"

#ifdef M64P_BIG_ENDIAN
  #define sl(mot) mot
#else
  #define sl(mot) (((mot & 0xFF) << 24) | ((mot & 0xFF00) <<  8) | ((mot & 0xFF0000) >>  8) | ((mot & 0xFF000000) >> 24))
#endif

/*******************************************************************************
 Functions called internally
 *******************************************************************************/

static char strBuff[1024];

static void swap_rom(unsigned char* localrom, int loadlength)
{
    unsigned char temp;
    int i;

    /* Btyeswap if .v64 image. */
    if (localrom[0] == 0x37)
    {
        for (i = 0; i < loadlength; i += 2)
        {
            temp = localrom[i];
            localrom[i] = localrom[i + 1];
            localrom[i + 1] = temp;
        }
    }
    /* Wordswap if .n64 image. */
    else if (localrom[0] == 0x40)
    {
        for (i = 0; i < loadlength; i += 4)
        {
            temp = localrom[i];
            localrom[i] = localrom[i + 3];
            localrom[i + 3] = temp;
            temp = localrom[i + 1];
            localrom[i + 1] = localrom[i + 2];
            localrom[i + 2] = temp;
        }
    }
}

static char * trim(char *str)
{
    unsigned int i;
    char *p = str;

    while (isspace(*p))
        p++;

    if (str != p)
    {
        for (i = 0; i <= strlen(p); ++i)
            str[i] = p[i];
    }

    p = str + strlen(str) - 1;
    if (p > str)
    {
        while (isspace(*p))
            p--;
        p[1] = '\0';
    }

    return str;
}

/*******************************************************************************
 Functions called automatically by JNI framework
 *******************************************************************************/

static JavaVM* mVm;
static void* mReserved;

// Library init
extern jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
    mVm = vm;
    mReserved = reserved;
    return JNI_VERSION_1_4;
}

/*******************************************************************************
 Functions called by Java code
 *******************************************************************************/

// Library handles
static void *handleAEI;         // libae-imports.so
static void *handleSDL;         // libSDL2.so
static void *handleCore;        // libcore.so
static void *handleFront;       // libfront-end.so

// Function types
typedef jint        (*pJNI_OnLoad)      (JavaVM* vm, void* reserved);
typedef int         (*pAeiInit)         (JNIEnv* env, jclass cls);
typedef int         (*pSdlInit)         (JNIEnv* env, jclass cls);
typedef void        (*pVoidFunc)        ();
typedef void        (*pSdlOnResize)     (JNIEnv* env, jclass jcls, jint width, jint height, jint format);
typedef m64p_error  (*pCoreDoCommand)   (m64p_command, int, void *);
typedef int         (*pFrontMain)       (int argc, char* argv[]);

// Function pointers
static pAeiInit         aeiInit         = NULL;
static pSdlInit         sdlInit         = NULL;
static pVoidFunc        sdlMainReady    = NULL;
static pSdlOnResize     sdlOnResize     = NULL;
static pCoreDoCommand   coreDoCommand   = NULL;
static pFrontMain       frontMain       = NULL;

extern "C" DECLSPEC void SDLCALL Java_paulscode_android_mupen64plusae_CoreInterfaceNative_loadLibraries(JNIEnv* env, jclass cls)
{
    LOGI("Loading native libraries");

    // TODO: Pass the library path as a function argument
    const char* pathAEI = "/data/data/paulscode.android.mupen64plusae/lib/libae-imports.so";
    const char* pathSDL = "/data/data/paulscode.android.mupen64plusae/lib/libSDL2.so";
    const char* pathCore = "/data/data/paulscode.android.mupen64plusae/lib/libcore.so";
    const char* pathFront = "/data/data/paulscode.android.mupen64plusae/lib/libfront-end.so";

    // Open shared libraries
    handleAEI = dlopen(pathAEI, RTLD_NOW);
    handleSDL = dlopen(pathSDL, RTLD_NOW);
    handleCore = dlopen(pathCore, RTLD_NOW);
    handleFront = dlopen(pathFront, RTLD_NOW);

    // Make sure we don't have any typos
    if (!handleAEI || !handleSDL || !handleCore || !handleFront)
    {
        LOGE("Could not load libraries: be sure the paths are correct");
    }

    // Find and call the JNI_OnLoad functions manually since we aren't loading the libraries from Java
    pJNI_OnLoad JNI_OnLoad0 = (pJNI_OnLoad) dlsym(handleAEI, "JNI_OnLoad");
    pJNI_OnLoad JNI_OnLoad1 = (pJNI_OnLoad) dlsym(handleSDL, "JNI_OnLoad");
    JNI_OnLoad0(mVm, mReserved);
    JNI_OnLoad1(mVm, mReserved);
    JNI_OnLoad0 = NULL;
    JNI_OnLoad1 = NULL;

    // Find library functions
    aeiInit         = (pAeiInit)        dlsym(handleAEI,    "SDL_Android_Init_Extras");
    sdlInit         = (pSdlInit)        dlsym(handleSDL,    "SDL_Android_Init");
    sdlMainReady    = (pVoidFunc)       dlsym(handleSDL,    "SDL_SetMainReady");
    sdlOnResize     = (pSdlOnResize)    dlsym(handleSDL,    "Java_org_libsdl_app_SDLActivity_onNativeResize");
    coreDoCommand   = (pCoreDoCommand)  dlsym(handleCore,   "CoreDoCommand");
    frontMain       = (pFrontMain)      dlsym(handleFront,  "SDL_main");

    // Make sure we don't have any typos
    if (!aeiInit || !sdlInit || !sdlMainReady || !sdlOnResize || !coreDoCommand || !frontMain)
    {
        LOGE("Could not load library functions: be sure they are named and typedef'd correctly");
    }
}

extern "C" DECLSPEC void SDLCALL Java_paulscode_android_mupen64plusae_CoreInterfaceNative_unloadLibraries(JNIEnv* env, jclass cls)
{
    LOGI("Unloading native libraries");

    // Nullify function pointers
    aeiInit         = NULL;
    sdlInit         = NULL;
    sdlMainReady    = NULL;
    sdlOnResize     = NULL;
    coreDoCommand   = NULL;
    frontMain       = NULL;

    // Close shared libraries
    if (handleFront) dlclose(handleFront);
    if (handleCore)  dlclose(handleCore);
    if (handleSDL)   dlclose(handleSDL);
    if (handleAEI)   dlclose(handleAEI);

    // Nullify handles
    handleFront     = NULL;
    handleCore      = NULL;
    handleSDL       = NULL;
    handleAEI       = NULL;
}

extern "C" DECLSPEC void SDLCALL Java_paulscode_android_mupen64plusae_CoreInterfaceNative_sdlInit(JNIEnv* env, jclass cls, jobjectArray jargv)
{
    // Initialize dependencies
    aeiInit(env, cls);
    sdlInit(env, cls);
    sdlMainReady();

    // Repackage the command-line args
    int argc = env->GetArrayLength(jargv);
    char **argv = (char **) malloc(sizeof(char *) * argc);
    for (int i = 0; i < argc; i++)
    {
        jstring jarg = (jstring) env->GetObjectArrayElement(jargv, i);
        const char *arg = env->GetStringUTFChars(jarg, 0);
        argv[i] = strdup(arg);
        env->ReleaseStringUTFChars(jarg, arg);
    }

    // Launch main emulator loop (continues until emuStop is called)
    frontMain(argc, argv);
}

extern "C" DECLSPEC void SDLCALL Java_paulscode_android_mupen64plusae_CoreInterfaceNative_sdlOnResize(JNIEnv* env, jclass jcls, jint width, jint height, jint format)
{
    sdlOnResize(env, jcls, width, height, format);
}

extern "C" DECLSPEC void Java_paulscode_android_mupen64plusae_CoreInterfaceNative_emuGameShark(JNIEnv* env, jclass cls, jboolean pressed)
{
    int p = pressed == JNI_TRUE ? 1 : 0;
    coreDoCommand(M64CMD_CORE_STATE_SET, M64CORE_INPUT_GAMESHARK, &p);
}

extern "C" DECLSPEC void Java_paulscode_android_mupen64plusae_CoreInterfaceNative_emuPause(JNIEnv* env, jclass cls)
{
    coreDoCommand(M64CMD_PAUSE, 0, NULL);
}

extern "C" DECLSPEC void Java_paulscode_android_mupen64plusae_CoreInterfaceNative_emuResume(JNIEnv* env, jclass cls)
{
    coreDoCommand(M64CMD_RESUME, 0, NULL);
}

extern "C" DECLSPEC void Java_paulscode_android_mupen64plusae_CoreInterfaceNative_emuStop(JNIEnv* env, jclass cls)
{
    coreDoCommand(M64CMD_STOP, 0, NULL);
}

extern "C" DECLSPEC void Java_paulscode_android_mupen64plusae_CoreInterfaceNative_emuAdvanceFrame(JNIEnv* env, jclass cls)
{
    coreDoCommand(M64CMD_ADVANCE_FRAME, 0, NULL);
}

extern "C" DECLSPEC void Java_paulscode_android_mupen64plusae_CoreInterfaceNative_emuSetSpeed(JNIEnv* env, jclass cls, jint percent)
{
    int speed_factor = (int) percent;
    coreDoCommand(M64CMD_CORE_STATE_SET, M64CORE_SPEED_FACTOR, &speed_factor);
}

extern "C" DECLSPEC void Java_paulscode_android_mupen64plusae_CoreInterfaceNative_emuSetSlot(JNIEnv* env, jclass cls, jint slotID)
{
    coreDoCommand(M64CMD_STATE_SET_SLOT, (int) slotID, NULL);
}

extern "C" DECLSPEC void Java_paulscode_android_mupen64plusae_CoreInterfaceNative_emuLoadSlot(JNIEnv* env, jclass cls)
{
    coreDoCommand(M64CMD_STATE_LOAD, 0, NULL);
}

extern "C" DECLSPEC void Java_paulscode_android_mupen64plusae_CoreInterfaceNative_emuSaveSlot(JNIEnv* env, jclass cls)
{
    coreDoCommand(M64CMD_STATE_SAVE, 1, NULL);
}

extern "C" DECLSPEC void Java_paulscode_android_mupen64plusae_CoreInterfaceNative_emuLoadFile(JNIEnv* env, jclass cls, jstring filename)
{
    const char *nativeString = env->GetStringUTFChars(filename, 0);
    coreDoCommand(M64CMD_STATE_LOAD, 0, (void *) nativeString);
    env->ReleaseStringUTFChars(filename, nativeString);
}

extern "C" DECLSPEC void Java_paulscode_android_mupen64plusae_CoreInterfaceNative_emuSaveFile(JNIEnv* env, jclass cls, jstring filename)
{
    const char *nativeString = env->GetStringUTFChars(filename, 0);
    coreDoCommand(M64CMD_STATE_SAVE, 1, (void *) nativeString);
    env->ReleaseStringUTFChars(filename, nativeString);
}

extern "C" DECLSPEC jint Java_paulscode_android_mupen64plusae_CoreInterfaceNative_emuGetState(JNIEnv* env, jclass cls)
{
    int state = 0;
    coreDoCommand(M64CMD_CORE_STATE_QUERY, M64CORE_EMU_STATE, &state);
    if (state == M64EMU_STOPPED)
        return (jint) 1;
    else if (state == M64EMU_RUNNING)
        return (jint) 2;
    else if (state == M64EMU_PAUSED)
        return (jint) 3;
    else
        return (jint) 0;
}
