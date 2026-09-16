// SPDX-License-Identifier: GPL-3.0-or-later
//
// JNI side of org.wiicompiled.quest.QuestSurface. Compiled into the product
// library (libmain.so) so the natives resolve once SDLActivity has loaded it,
// which happens before the activity creates its surface.

#include <aurora/android.h>

#include <jni.h>

extern "C" JNIEXPORT void JNICALL
Java_org_wiicompiled_quest_QuestSurface_nativeBeginSurfaceMutation(JNIEnv*, jobject) {
    aurora_android_begin_surface_mutation();
}

extern "C" JNIEXPORT void JNICALL
Java_org_wiicompiled_quest_QuestSurface_nativeEndSurfaceMutation(JNIEnv*, jobject, jboolean ready) {
    aurora_android_end_surface_mutation(ready == JNI_TRUE);
}
