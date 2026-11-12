/* JNI bridge: dev.forge.ForgeModel (Kotlin) -> the stable C ABI in forge.h. Built by CMake with
   -DFORGE_JNI=ON (Android NDK or any host with jni.h). One Java object owns one forge_model*. */
#include <jni.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "forge.h"

static void throw_forge(JNIEnv* env, const char* what) {
    jclass c = (*env)->FindClass(env, "java/lang/IllegalStateException");
    const char* e = forge_last_error();
    char buf[512]; snprintf(buf, sizeof buf, "%s: %s", what, e && *e ? e : "unknown error");
    (*env)->ThrowNew(env, c, buf);
}
#define M(h) ((forge_model*)(intptr_t)(h))

JNIEXPORT jstring JNICALL Java_dev_forge_ForgeModel_nativeVersion(JNIEnv* env, jclass c) { (void)c; return (*env)->NewStringUTF(env, forge_version()); }
JNIEXPORT jint JNICALL Java_dev_forge_ForgeModel_nativeAbi(JNIEnv* env, jclass c) { (void)env; (void)c; return (jint)forge_abi_version(); }

JNIEXPORT jlong JNICALL Java_dev_forge_ForgeModel_nativeOpen(JNIEnv* env, jclass c, jstring path, jlong ctx, jboolean verify) {
    (void)c; const char* p = (*env)->GetStringUTFChars(env, path, NULL);
    forge_model* m = forge_open(p, (int64_t)ctx, verify ? 1 : 0);
    (*env)->ReleaseStringUTFChars(env, path, p);
    if (!m) throw_forge(env, "forge_open");
    return (jlong)(intptr_t)m;
}
JNIEXPORT void JNICALL Java_dev_forge_ForgeModel_nativeClose(JNIEnv* env, jclass c, jlong h) { (void)env; (void)c; if (h) forge_close(M(h)); }
JNIEXPORT void JNICALL Java_dev_forge_ForgeModel_nativeReset(JNIEnv* env, jclass c, jlong h) { (void)env; (void)c; forge_reset(M(h)); }
JNIEXPORT jlong JNICALL Java_dev_forge_ForgeModel_nativeVocab(JNIEnv* env, jclass c, jlong h) { (void)env; (void)c; return (jlong)forge_vocab_size(M(h)); }
JNIEXPORT jlong JNICALL Java_dev_forge_ForgeModel_nativePosition(JNIEnv* env, jclass c, jlong h) { (void)env; (void)c; return (jlong)forge_position(M(h)); }
JNIEXPORT void JNICALL Java_dev_forge_ForgeModel_nativeRewind(JNIEnv* env, jclass c, jlong h, jlong pos) {
    (void)c; if (forge_rewind(M(h), (int64_t)pos) != 0) throw_forge(env, "forge_rewind"); }

JNIEXPORT jintArray JNICALL Java_dev_forge_ForgeModel_nativeTokenize(JNIEnv* env, jclass c, jlong h, jstring text, jboolean bos) {
    (void)c; const char* t = (*env)->GetStringUTFChars(env, text, NULL);
    int64_t cap = (int64_t)strlen(t) + 8; int32_t* ids = malloc((size_t)cap * 4);
    int64_t n = forge_tokenize(M(h), t, bos ? 1 : 0, ids, cap);
    (*env)->ReleaseStringUTFChars(env, text, t);
    if (n < 0) { free(ids); throw_forge(env, "forge_tokenize"); return NULL; }
    jintArray r = (*env)->NewIntArray(env, (jsize)n); (*env)->SetIntArrayRegion(env, r, 0, (jsize)n, ids); free(ids); return r;
}
JNIEXPORT jstring JNICALL Java_dev_forge_ForgeModel_nativeDetokenize(JNIEnv* env, jclass c, jlong h, jintArray ids) {
    (void)c; jsize n = (*env)->GetArrayLength(env, ids); jint* p = (*env)->GetIntArrayElements(env, ids, NULL);
    int64_t cap = (int64_t)n * 32 + 16; char* out = malloc((size_t)cap);
    int64_t k = forge_detokenize(M(h), (const int32_t*)p, n, out, cap);
    (*env)->ReleaseIntArrayElements(env, ids, p, JNI_ABORT);
    if (k < 0) { free(out); throw_forge(env, "forge_detokenize"); return NULL; }
    out[k < cap ? k : cap - 1] = 0; jstring s = (*env)->NewStringUTF(env, out); free(out); return s;
}
JNIEXPORT jfloatArray JNICALL Java_dev_forge_ForgeModel_nativeForward(JNIEnv* env, jclass c, jlong h, jintArray ids) {
    (void)c; int64_t v = forge_vocab_size(M(h));
    jsize n = (*env)->GetArrayLength(env, ids); jint* p = (*env)->GetIntArrayElements(env, ids, NULL);
    float* lg = malloc((size_t)v * 4);
    int rc = forge_forward(M(h), (const int32_t*)p, n, lg, v);
    (*env)->ReleaseIntArrayElements(env, ids, p, JNI_ABORT);
    if (rc != 0) { free(lg); throw_forge(env, "forge_forward"); return NULL; }
    jfloatArray r = (*env)->NewFloatArray(env, (jsize)v); (*env)->SetFloatArrayRegion(env, r, 0, (jsize)v, lg); free(lg); return r;
}

typedef struct { JNIEnv* env; jobject cb; jmethodID m; } cbctx;
static int on_token(int32_t tok, const char* text, void* u) {
    cbctx* c = u; if (!c->cb) return 0;
    jstring s = (*c->env)->NewStringUTF(c->env, text ? text : "");
    jboolean stop = (*c->env)->CallBooleanMethod(c->env, c->cb, c->m, (jint)tok, s);
    (*c->env)->DeleteLocalRef(c->env, s);
    return (*c->env)->ExceptionCheck(c->env) || stop;
}
JNIEXPORT jintArray JNICALL Java_dev_forge_ForgeModel_nativeGenerate(JNIEnv* env, jclass c, jlong h, jintArray prompt, jint max_new,
        jfloat temp, jint top_k, jfloat top_p, jfloat min_p, jfloat rep, jlong seed, jobject cb) {
    (void)c; forge_sampling s = {temp, top_k, top_p, min_p, rep, (uint64_t)seed};
    jsize n = (*env)->GetArrayLength(env, prompt); jint* p = (*env)->GetIntArrayElements(env, prompt, NULL);
    int32_t* out = malloc((size_t)(max_new > 0 ? max_new : 1) * 4);
    cbctx cc = {env, cb, NULL};
    if (cb) { jclass k = (*env)->GetObjectClass(env, cb); cc.m = (*env)->GetMethodID(env, k, "onToken", "(ILjava/lang/String;)Z"); }
    int64_t k = forge_generate(M(h), (const int32_t*)p, n, max_new, &s, out, max_new, cb ? on_token : NULL, &cc);
    (*env)->ReleaseIntArrayElements(env, prompt, p, JNI_ABORT);
    if (k < 0) { free(out); if (!(*env)->ExceptionCheck(env)) throw_forge(env, "forge_generate"); return NULL; }
    jintArray r = (*env)->NewIntArray(env, (jsize)k); (*env)->SetIntArrayRegion(env, r, 0, (jsize)k, out); free(out); return r;
}
