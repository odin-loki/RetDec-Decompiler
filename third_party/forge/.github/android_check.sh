#!/bin/sh
# Runs the --emit android goldens on the emulator's /system/bin/sh and the native test_script binary.
set -eu
d=/data/local/tmp/forge; adb shell "rm -rf $d && mkdir -p $d"; fail=0
adb push emit $d/ >/dev/null
for g in functions flat batch; do
  got=$(adb shell "cd $d/emit && /system/bin/sh ./$g.sh; echo RC=\$?" | tr -d '\r')
  rc=$(echo "$got" | sed -n 's/^RC=//p'); got=$(echo "$got" | sed '/^RC=/d')
  # emit/$g.want comes from the Linux host interpreter, so its platform() line says linux.
  want=$(sed 's/plat=linux/plat=android/' emit/$g.want)
  if [ "$got" = "$want" ] && [ "$rc" = "$(cat emit/$g.rc)" ]; then echo "PASS emit android $g"; else echo "FAIL emit android $g (rc $rc)"; echo "$got"; fail=1; fi
done
libcxx=$(find -L "$ANDROID_NDK_LATEST_HOME/" -path "*x86_64-linux-android/libc++_shared.so" | head -1)
adb push "$libcxx" build/android-x86_64/test_script build/android-x86_64/libhello_plugin.so $d/ >/dev/null
adb push tests $d/ >/dev/null
adb shell "mkdir -p $d/templates" && adb push import/templates/. $d/templates/ >/dev/null   # model templates (tiny model in ask_local_model)
adb shell "cd $d && LD_LIBRARY_PATH=$d TMPDIR=$d FORGE_TEMPLATES=$d/templates FORGE_TEST_PLUGIN_PATH=$d/libhello_plugin.so ./test_script; echo RC=\$?" | tr -d '\r' | tee out.txt
grep -q '^RC=0$' out.txt || fail=1
# forge_mobile (Android backend, bridge, services), forge_text/locale (tzdata bundle, Bionic locale paths),
# forge::os (core, extension, system information), support utilities and the load balancer. 77 = skip.
for t in test_mobile test_locale test_text test_os test_os_ext test_os_sys test_util test_loadbalancer; do
  adb push build/android-x86_64/$t $d/ >/dev/null
  adb shell "cd $d && LD_LIBRARY_PATH=$d TMPDIR=$d FORGE_THREADS=2 timeout 600 ./$t; echo RC=\$?" | tr -d '\r' | tee $t.txt
  rc=$(sed -n 's/^RC=//p' $t.txt | tail -1)
  case "$rc" in 0) echo "PASS $t";; 77) echo "SKIP $t";; *) echo "FAIL $t (rc=$rc)"; fail=1;; esac
done
exit $fail
