#!/bin/sh
# Emulator debugging pass for Android (x86_64 AVD). Exit 77 when no device/SDK.
#   android_emulator.sh native <build-dir>   push test_* binaries + libc++_shared.so, run each on the device
#   android_emulator.sh apk <build-dir>      build the example APK without gradle (kotlinc + d8 + aapt2 +
#                                            apksigner), install, launch, and check the JNI library loaded
# Env: ANDROID_HOME (SDK), ANDROID_NDK_HOME, JAVA_HOME, KOTLINC (path to kotlinc).
set -eu
src=$(cd "$(dirname "$0")/.." && pwd)
sdk=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}; adb=${ADB:-$sdk/platform-tools/adb}
[ -x "$adb" ] && "$adb" get-state >/dev/null 2>&1 || { echo "SKIP: no adb device"; exit 77; }
b=$(cd "$2" && pwd); abi=$("$adb" shell getprop ro.product.cpu.abi | tr -d '\r')
case "$1" in
native)
  libcxx=$(find -L "$ANDROID_NDK_HOME/" -path "*$( [ "$abi" = x86_64 ] && echo x86_64-linux-android || echo aarch64-linux-android)/libc++_shared.so" | head -1)
  d=/data/local/tmp/forge; "$adb" shell "rm -rf $d && mkdir -p $d"
  "$adb" push "$libcxx" $d/ >/dev/null
  for f in "$b"/lib*.so; do [ -f "$f" ] && "$adb" push "$f" $d/ >/dev/null; done
  "$adb" push "$src/tests/data" $d/tests/ >/dev/null 2>&1 || true
  "$adb" push "$src/import/templates" $d/templates >/dev/null
  fail=0; skip=0; pass=0
  for t in "$b"/test_* "$b"/example_*; do
    [ -x "$t" ] && [ -f "$t" ] || continue; n=$(basename "$t")
    rc=; out=
    for attempt in 1 2 3; do   # adb on a TCG emulator drops under memory pressure: wait and retry, not a test failure
      timeout 300 "$adb" wait-for-device
      "$adb" push "$t" $d/ >/dev/null || { sleep 10; continue; }
      out=$("$adb" shell "cd $d && LD_LIBRARY_PATH=$d TMPDIR=$d FORGE_TEMPLATES=$d/templates FORGE_THREADS=${FORGE_THREADS:-2} timeout ${FORGE_TEST_TIMEOUT:-600} ./$n; echo RC=\$?" 2>&1 | tr -d '\r')
      rc=$(echo "$out" | sed -n 's/^RC=//p' | tail -1)
      [ -n "$rc" ] && break; echo "  (adb lost during $n, retry $attempt)"; sleep 10
    done
    case "$rc" in 0) pass=$((pass+1)); echo "PASS $n";; 77) skip=$((skip+1)); echo "SKIP $n";;
      *) fail=$((fail+1)); echo "FAIL $n (rc=$rc)"; echo "$out" | grep -E "FAIL|fail|error|Error|abort" | head -8;; esac
    "$adb" shell rm -f $d/$n
  done
  echo "android/$abi: $pass passed, $skip skipped, $fail failed"; [ $fail -eq 0 ];;
apk)
  jar=$sdk/platforms/android-34/android.jar; bt=$(ls -d "$sdk"/build-tools/* | tail -1); w=$(mktemp -d)
  "${KOTLINC:-kotlinc}" "$src/bindings/android/forge/src/main/kotlin" "$src/bindings/android/example/src/main/kotlin" \
    -cp "$jar" -d "$w/classes" -jvm-target 17 -nowarn
  ktstd=$(dirname "$(command -v "${KOTLINC:-kotlinc}")")/../lib/kotlin-stdlib.jar
  "$bt/d8" --min-api 26 --lib "$jar" --output "$w" $(find "$w/classes" -name '*.class') "$ktstd"
  mkdir -p "$w/apk/lib/$abi"
  cp "$b/libforge_jni.so" "$b/libforge.so" "$w/apk/lib/$abi/"
  cp "$(find -L "$ANDROID_NDK_HOME/" -path "*$( [ "$abi" = x86_64 ] && echo x86_64-linux-android || echo aarch64-linux-android)/libc++_shared.so" | head -1)" "$w/apk/lib/$abi/"
  sed 's/<manifest /<manifest package="dev.forge.example" /' "$src/bindings/android/example/src/main/AndroidManifest.xml" > "$w/AndroidManifest.xml"
  "$bt/aapt2" link -o "$w/base.apk" -I "$jar" --manifest "$w/AndroidManifest.xml" --min-sdk-version 26 --target-sdk-version 34
  (cd "$w" && cp classes.dex apk/ && cd apk && zip -qr ../base.apk classes.dex lib -0)
  "$bt/zipalign" -f -p 4 "$w/base.apk" "$w/aligned.apk"
  [ -f "$w/debug.jks" ] || keytool -genkeypair -keystore "$w/debug.jks" -storepass android -keypass android -alias d -keyalg RSA -dname CN=forge -validity 30 >/dev/null 2>&1
  "$bt/apksigner" sign --ks "$w/debug.jks" --ks-pass pass:android --out "$w/forge-example.apk" "$w/aligned.apk"
  "$adb" install -r "$w/forge-example.apk"
  "$adb" logcat -c; "$adb" shell am start -W -n dev.forge.example/.MainActivity
  sleep 5
  log=$("$adb" logcat -d | grep -E "dev.forge|FATAL|UnsatisfiedLink|forge" | head -40)
  echo "$log"
  pid=$("$adb" shell pidof dev.forge.example | tr -d '\r')
  echo "$log" | grep -q "FATAL\|UnsatisfiedLinkError" && { echo "FAIL: app crashed"; exit 1; }
  [ -n "$pid" ] || { echo "FAIL: app not running"; exit 1; }
  "$adb" exec-out uiautomator dump /dev/tty 2>/dev/null | grep -o 'text="FORGE[^"]*"' | head -2
  echo "PASS: example app running (pid $pid), $w/forge-example.apk";;
*) echo "usage: $0 native|apk <android-build-dir>"; exit 2;;
esac
