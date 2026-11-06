#!/bin/sh
# End-to-end tests. Everything happens in a temporary root; no root needed.
#   sh tests/run.sh            (or: make check)
set -u

here=$(cd "$(dirname "$0")" && pwd)
FPM_BIN=${FPM_BIN:-$here/../fpm}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

R=$T/root
cp -r "$here/repo" "$T/repo"
mkdir -p "$R"
cat > "$T/fpm.conf" <<CONF
repos = $T/repo
jobs = 2
cflags = -O2
# The test root has no C library of its own.
shlib_ignore = libc.so.6
CONF

pass=0 fail=0
ok()   { pass=$((pass + 1)); printf '  ok    %s\n' "$1"; }
bad()  { fail=$((fail + 1)); printf '  FAIL  %s\n' "$1"; }
check() { desc=$1; shift; if "$@" >/dev/null 2>&1; then ok "$desc"; else bad "$desc"; fi; }
refuse() { desc=$1; shift; if "$@" >/dev/null 2>&1; then bad "$desc"; else ok "$desc"; fi; }
fpm() { "$FPM_BIN" --root "$R" --config "$T/fpm.conf" "$@"; }
q() { fpm "$@" >/dev/null 2>&1; }
# has DESC PATTERN CMD...: CMD's output (stdout+stderr) contains PATTERN
has() { desc=$1 pat=$2; shift 2; out=$("$@" 2>&1); if printf '%s\n' "$out" | grep -q -- "$pat"; then ok "$desc"; else bad "$desc"; fi; }
bump() { sed -i "s/^version *=.*/version = $2/" "$T/repo/$1/recipe"; }

echo "install & dependencies"
check "install hello"                     q install hello
check "binary runs"                       sh -c "[ \"\$('$R/usr/bin/hello')\" = 'hello 1.0' ]"
check "dependency greet-data installed"   test -f "$R/usr/share/greet/message"
check "only hello is explicit"            sh -c "[ \"\$(\"$FPM_BIN\" --root '$R' --config '$T/fpm.conf' list -e)\" = 'hello 1.0-1' ]"
check "post-install hook ran"             test -f "$R/var/lib/hello/hook-ran"
check "owner lookup"                      sh -c "\"$FPM_BIN\" --root '$R' --config '$T/fpm.conf' owner /usr/bin/hello | grep -q 'owned by hello'"
check "binaries are not hashed"           grep -q '^f - usr/bin/hello$' "$R/var/lib/fpm/installed/hello/files"
check "/etc files are hashed"             grep -Eq '^f [0-9a-f]{64} etc/hello.conf$' "$R/var/lib/fpm/installed/hello/files"
check "cached package exists"             test -f "$R/var/cache/fpm/pkgs/hello-1.0-1.fpm.tar.zst"
check "reinstall is a no-op"              sh -c "\"$FPM_BIN\" --root '$R' --config '$T/fpm.conf' install hello | grep -q 'up to date'"

echo "safety checks"
refuse "file conflict is refused"         q install clash
check  "conflict left hello intact"       sh -c "[ \"\$('$R/usr/bin/hello')\" = 'hello 1.0' ]"
refuse "dependency cycle is refused"      q install cyc-a
refuse "unsatisfiable version refused"    q install needs-new
refuse "required package can't be removed" q remove greet-data
cp "$T/repo/hello/hello.c" "$T/hello.c.orig"
echo '/* tampered */' >> "$T/repo/hello/hello.c"
refuse "bad source checksum is refused"   q build --rebuild hello
cp "$T/hello.c.orig" "$T/repo/hello/hello.c"
check  "dry run changes nothing"          sh -c "\"$FPM_BIN\" --root '$R' --config '$T/fpm.conf' -n remove hello >/dev/null && test -f '$R/usr/bin/hello'"

echo "upgrade & config files"
echo "greeting = Hi" > "$R/etc/hello.conf"
bump hello 1.1
check "upgrade"                           q upgrade
check "new version runs"                  sh -c "[ \"\$('$R/usr/bin/hello')\" = 'hello 1.1' ]"
check "edited config kept"                grep -q Hi "$R/etc/hello.conf"
check "new config saved as .fpmnew"       grep -q Hello "$R/etc/hello.conf.fpmnew"

echo "generations & rollback"
check "history lists 2 generations"       sh -c "[ \$(\"$FPM_BIN\" --root '$R' --config '$T/fpm.conf' history | wc -l) -eq 2 ]"
check "rollback"                          q rollback
check "old version is back"               sh -c "[ \"\$('$R/usr/bin/hello')\" = 'hello 1.0' ]"
check "rollback recorded as generation 3" sh -c "\"$FPM_BIN\" --root '$R' --config '$T/fpm.conf' history | grep -q '^\* *3 .*rollback'"
check "roll forward to generation 2"      q rollback 2
check "1.1 is back"                       sh -c "[ \"\$('$R/usr/bin/hello')\" = 'hello 1.1' ]"

echo "remove & autoremove"
check "remove hello"                      q remove hello
check "binary gone"                       test ! -e "$R/usr/bin/hello"
check "edited config saved as .fpmsave"   test -f "$R/etc/hello.conf.fpmsave"
check "autoremove"                        q autoremove
check "orphaned dependency removed"       test ! -e "$R/usr/share/greet"
check "nothing installed"                 sh -c "[ -z \"\$(\"$FPM_BIN\" --root '$R' --config '$T/fpm.conf' list)\" ]"

echo "clean"
check "clean --keep 1"                    q clean --keep 1
check "unreferenced packages deleted"     test ! -f "$R/var/cache/fpm/pkgs/hello-1.1-1.fpm.tar.zst"

echo "parallel builds"
start=$(date +%s)
check "install two 2-second builds with -P 2" q -P 2 install slow-both
elapsed=$(( $(date +%s) - start ))
check "they built at the same time (${elapsed}s < 4s)" test "$elapsed" -lt 4
check "build logs written"                test -f "$R/var/cache/fpm/logs/slow-a.log"
check "both installed"                    test -f "$R/usr/share/slow/a" -a -f "$R/usr/share/slow/b"

echo "git sources"
G=$T/gitsrc
git init -q "$G"
echo "first" > "$G/greeting.txt"
git -C "$G" add greeting.txt
git -C "$G" -c user.name=t -c user.email=t@t commit -qm first
first=$(git -C "$G" rev-parse HEAD)
echo "second" > "$G/greeting.txt"
git -C "$G" -c user.name=t -c user.email=t@t commit -qam second
sed -i "s|@GITURL@|file://$G|; s|@COMMIT@|$first|" "$T/repo/gitpkg/recipe"
check "install from a pinned git commit"  q install gitpkg
check "pinned commit was checked out"     grep -qx first "$R/usr/share/gitpkg/greeting.txt"
sed -i "s|#commit=$first|#branch=master|" "$T/repo/gitpkg/recipe"
git -C "$G" branch -m master 2>/dev/null
check "rebuild following a branch"        q install --reinstall --rebuild gitpkg
check "branch head was checked out"       grep -qx second "$R/usr/share/gitpkg/greeting.txt"
git -C "$G" checkout -qb feature
echo "third" > "$G/greeting.txt"
git -C "$G" -c user.name=t -c user.email=t@t commit -qam third
git -C "$G" checkout -q master
sed -i "s|#branch=master|#branch=feature|" "$T/repo/gitpkg/recipe"
check "rebuild from a non-default branch" q install --reinstall --rebuild gitpkg
check "that branch was checked out"       grep -qx third "$R/usr/share/gitpkg/greeting.txt"
sed -i "s|#branch=feature|#commit=0000000000000000000000000000000000000000|" "$T/repo/gitpkg/recipe"
refuse "unknown commit is refused"        q install --reinstall --rebuild gitpkg

echo "split packages"
check "install a subpackage"              q install libfoo-dev
check "its parent came as a dependency"   test -f "$R/usr/lib/libfoo.so.1"
check "header belongs to libfoo-dev"      sh -c "\"$FPM_BIN\" --root '$R' --config '$T/fpm.conf' owner /usr/include/foo.h | grep -q 'owned by libfoo-dev'"
check "static lib belongs to libfoo-dev"  sh -c "\"$FPM_BIN\" --root '$R' --config '$T/fpm.conf' owner /usr/lib/libfoo.a | grep -q 'owned by libfoo-dev'"
check "shared lib belongs to libfoo"      sh -c "\"$FPM_BIN\" --root '$R' --config '$T/fpm.conf' owner /usr/lib/libfoo.so.1 | grep -q 'owned by libfoo '"
check "unrequested libfoo-doc not installed" test ! -e "$R/usr/share/doc/libfoo"
check "libfoo-doc was built once with the rest" test -f "$R/var/cache/fpm/pkgs/libfoo-doc-1.0-1.fpm.tar.zst"
check "installing libfoo-doc needs no rebuild" sh -c "! \"$FPM_BIN\" --root '$R' --config '$T/fpm.conf' install libfoo-doc | grep -q 'Building'"
check "libfoo-doc installed"              test -f "$R/usr/share/doc/libfoo/README"

echo "crash repair"
check "install journal 1"                 q install journal
sed -i 's/^version = 1$/version = 2/' "$T/repo/journal/recipe"
refuse "upgrade killed mid-install"       env FPM_TEST_INTERRUPT=1 "$FPM_BIN" --root "$R" --config "$T/fpm.conf" upgrade
check "journal left behind"               test -f "$R/var/lib/fpm/pending"
check "list warns about it"               sh -c "\"$FPM_BIN\" --root '$R' --config '$T/fpm.conf' list 2>&1 | grep -q 'interrupted'"
check "repair"                            q repair
check "database now at version 2"         sh -c "\"$FPM_BIN\" --root '$R' --config '$T/fpm.conf' list | grep -qx 'journal 2-1'"
check "old version's file removed"        test ! -e "$R/usr/share/journal/v1"
check "new version's file present"        test -f "$R/usr/share/journal/v2"
check "journal cleared"                   test ! -e "$R/var/lib/fpm/pending"

echo "provides, conflicts, replaces"
mkdir -p "$R/etc/fpm/hooks"
printf 'paths = usr/bin/*\nexec = echo ran >> "$ROOT/var/hook.log"\nchroot = no\n' > "$R/etc/fpm/hooks/log.hook"
check "dependency on a virtual package"   q install needs-sh
check "its provider was installed"        test -f "$R/usr/bin/busysh"
check "hook ran once for the transaction" sh -c "[ \$(wc -l < '$R/var/hook.log') -eq 1 ]"
refuse "provider can't be removed while needed" q remove busysh
check "install nano-a"                    q install nano-a
has   "conflicting nano-b is refused"     "conflicts with the installed nano-a" fpm install nano-b
check "install oldname"                   q install oldname
check "newname replaces oldname"          q install newname
check "oldname was removed"               sh -c "! \"$FPM_BIN\" --root '$R' --config '$T/fpm.conf' list | grep -q oldname"
check "its file now belongs to newname"   grep -qx new "$R/usr/bin/tool"

echo "hold"
check "hold busysh"                       q hold busysh
bump busysh 2
has   "upgrade skips it"                  "busysh is held" fpm upgrade
check "still version 1"                   sh -c "\"$FPM_BIN\" --root '$R' --config '$T/fpm.conf' list | grep -qx 'busysh 1-1 \[held\]'"
check "unhold busysh"                     q unhold busysh
check "upgrade now takes it"              q upgrade
check "now version 2"                     sh -c "\"$FPM_BIN\" --root '$R' --config '$T/fpm.conf' list | grep -qx 'busysh 2-1'"

echo "queries"
has   "deps shows the provider"           "provided by busysh" fpm deps needs-sh
has   "rdeps finds who needs it"          "needs-sh" fpm rdeps busysh
check "orphans: none yet"                 sh -c "[ -z \"\$(\"$FPM_BIN\" --root '$R' --config '$T/fpm.conf' orphans)\" ]"
check "check passes"                      q check
rm "$R/usr/bin/nano-a"
has   "check reports a missing file"      "missing /usr/bin/nano-a" fpm check nano-a
refuse "check exits non-zero for it"      q check nano-a
has   "info shows where it comes from"    "build from recipe" fpm info libgreet
has   "show prints the build script"      "soname" fpm show libgreet

echo "binary repositories"
B=$T/builder SRV=$T/srv/main C1=$T/client EMPTY=$T/empty
mkdir -p "$B" "$SRV" "$C1" "$EMPTY" "$T/keys"
check "keygen"                            "$FPM_BIN" keygen "$T/keys/main"
check "private key is private"            sh -c "[ \$(stat -c %a '$T/keys/main.key') = 600 ]"
check "build packages for the repo"       "$FPM_BIN" --root "$B" --config "$T/fpm.conf" build greet-data hello libgreet greeter
cp "$B"/var/cache/fpm/pkgs/*.fpm.tar.zst "$SRV/"
check "index and sign the repo"           "$FPM_BIN" index "$SRV" --sign "$T/keys/main.key"
check "signature written"                 test -f "$SRV/index.fpm.zst.sig"
has   "greeter needs libgreet.so.1"       "shlib-requires = .*libgreet.so.1" zstd -dc "$SRV/index.fpm.zst"
cat > "$T/client.conf" <<CONF
shlib_ignore = libc.so.6
[recipes none]
path = $EMPTY
[repo main]
url = file://$SRV
key = $T/keys/main.pub
CONF
cli() { "$FPM_BIN" --root "$C1" --config "$T/client.conf" "$@"; }
check "sync verifies and stores the index" cli sync
has   "search finds repo packages"        "hello .*\[main\]" cli search hello
has   "install hello from the repo"       "Downloading hello" cli install hello
check "no build happened"                 test ! -d "$C1/var/cache/fpm/sources"
check "hello runs"                        test -x "$C1/usr/bin/hello"
check "install greeter"                   cli install greeter
check "libgreet came via its soname"      test -f "$C1/usr/lib/libgreet.so.1"
has   "libgreet can't be removed"         "required by: greeter" cli remove libgreet
cp "$SRV/index.fpm.zst" "$SRV/index.fpm.zst.sig" "$T/"
sleep 1
bump libgreet 2
check "build libgreet 2"                  "$FPM_BIN" --root "$B" --config "$T/fpm.conf" build libgreet
cp "$B/var/cache/fpm/pkgs/libgreet-2-1.fpm.tar.zst" "$SRV/"
check "re-index (reuses unchanged)"       "$FPM_BIN" index "$SRV" --sign "$T/keys/main.key"
check "sync the new index"                cli sync
has   "upgrade that drops a used soname is refused" "drops libgreet.so.1, which greeter still uses" cli upgrade
check "libgreet still at 1"               test -f "$C1/usr/lib/libgreet.so.1"
cp "$T/index.fpm.zst" "$T/index.fpm.zst.sig" "$SRV/"
has   "replayed old index is refused"     "older than the one you have" cli sync
check "evil keygen"                       "$FPM_BIN" keygen "$T/keys/evil"
check "index signed with the wrong key"   "$FPM_BIN" index "$SRV" --sign "$T/keys/evil.key"
has   "wrong signature is refused"        "SIGNATURE CHECK FAILED" cli sync
check "index without signature"           "$FPM_BIN" index "$SRV"
has   "missing signature is refused"      "has no signature" cli sync
check "re-sign properly"                  "$FPM_BIN" index "$SRV" --sign "$T/keys/main.key"
f=$(ls "$SRV"/greet-data-*.fpm.tar.zst)
cp "$f" "$T/greet-data.bak"
printf 'x' | dd of="$f" bs=1 seek=200 conv=notrunc 2>/dev/null
C2=$T/client2
mkdir -p "$C2"
cli2() { "$FPM_BIN" --root "$C2" --config "$T/client.conf" "$@"; }
check "fresh client syncs"                cli2 sync
has   "tampered archive is refused"       "corrupted or tampered" cli2 install greet-data
check "nothing was installed"             sh -c "[ -z \"\$(\"$FPM_BIN\" --root '$C2' --config '$T/client.conf' list)\" ]"

echo "every version in the index"
SRV2=$T/srv/multi C3=$T/client3
mkdir -p "$SRV2" "$C3"
cp "$T/greet-data.bak" "$SRV2/greet-data-1.0-1.fpm.tar.zst"
bump greet-data 2.0
check "build greet-data 2.0"              "$FPM_BIN" --root "$B" --config "$T/fpm.conf" build greet-data
cp "$B/var/cache/fpm/pkgs/greet-data-2.0-1.fpm.tar.zst" "$SRV2/"
has   "index lists both versions"         "2 package version" "$FPM_BIN" index "$SRV2" --sign "$T/keys/main.key"
sed "s|file://$SRV\$|file://$SRV2|" "$T/client.conf" > "$T/client3.conf"
cli3() { "$FPM_BIN" --root "$C3" --config "$T/client3.conf" "$@"; }
check "sync"                              cli3 sync
check "install takes the newest"          cli3 install greet-data
check "2.0 installed"                     sh -c "\"$FPM_BIN\" --root '$C3' --config '$T/client3.conf' list | grep -qx 'greet-data 2.0-1'"
check "install an older version by constraint" cli3 install greet-data=1.0
check "1.0 installed"                     sh -c "\"$FPM_BIN\" --root '$C3' --config '$T/client3.conf' list | grep -qx 'greet-data 1.0-1'"
has   "impossible constraint is explained" "no such version (newest: 2.0-1)" cli3 install "greet-data>=3"
check "search shows only the newest"      sh -c "out=\$(\"$FPM_BIN\" --root '$C3' --config '$T/client3.conf' search greet-data); echo \"\$out\" | grep -q 'greet-data 2.0-1' && ! echo \"\$out\" | grep -q '1.0-1'"
rm -f "$C3"/var/cache/fpm/pkgs/*
has   "rollback re-downloads what the cache lost" "Downloading greet-data 2.0-1" cli3 rollback
check "2.0 is back"                       sh -c "\"$FPM_BIN\" --root '$C3' --config '$T/client3.conf' list | grep -qx 'greet-data 2.0-1'"

echo "git submodules"
GS=$T/subsrc GP=$T/parentsrc
git init -q "$GS"
echo "from the submodule" > "$GS/sub.txt"
git -C "$GS" add sub.txt
git -C "$GS" -c user.name=t -c user.email=t@t commit -qm sub
git init -q "$GP"
git -C "$GP" -c protocol.file.allow=always submodule --quiet add "file://$GS" sub
git -C "$GP" -c user.name=t -c user.email=t@t commit -qm parent
sed -i "s|@GITURL@|file://$GP|; s|@COMMIT@|$(git -C "$GP" rev-parse HEAD)|" "$T/repo/gitsub/recipe"
check "build a repository with a submodule" q install gitsub
check "submodule content was there"       grep -qx "from the submodule" "$R/usr/share/gitsub/sub.txt"

echo "pre-remove scripts"
check "install removable"                 q install removable
check "remove it"                         q remove removable
check "script ran before files were removed" grep -qx before "$R/var/pre-remove-ran"
check "files are gone"                    test ! -e "$R/usr/share/removable"
check "install stubborn"                  q install stubborn
has   "failing pre-remove stops removal"  "pre-remove script failed" fpm remove stubborn
check "its files are still there"         test -f "$R/usr/share/stubborn/x"
check "--force removes it anyway"         q --force remove stubborn
check "now gone"                          test ! -e "$R/usr/share/stubborn/x"

echo "recipe review"
RV=$T/rv
mkdir -p "$RV"
cp -r "$here/review-repo" "$T/review-repo"
cat > "$T/review.conf" <<CONF
shlib_ignore = libc.so.6
[recipes community]
path = $T/review-repo
review = yes
CONF
rv() { "$FPM_BIN" --root "$RV" --config "$T/review.conf" "$@"; }
has   "unreviewed recipe is refused without a terminal" "not reviewed" rv install reviewed
check "nothing was built"                 test ! -e "$RV/usr/share/reviewed/ok"
has   "--yes shows it and builds"         "printf\|mkdir" rv --yes install reviewed
check "it was installed"                  test -f "$RV/usr/share/reviewed/ok"
check "unchanged recipe needs no review"  rv --reinstall install reviewed
echo 'echo changed' >> "$T/review-repo/reviewed/build"
has   "changed recipe must be reviewed again" "not reviewed" rv --reinstall --rebuild install reviewed

echo "untracked files"
RO=$T/overwrite-root
mkdir -p "$RO"
ro() { "$FPM_BIN" --root "$RO" --config "$T/fpm.conf" "$@" >/dev/null 2>&1; }
check  "install hello"                    ro install hello
refuse "--overwrite keeps package conflicts" ro --overwrite install clash
mkdir -p "$RO/usr/bin" && printf '#!/bin/sh\necho stray\n' > "$RO/usr/bin/adopted" && chmod 755 "$RO/usr/bin/adopted"
refuse "untracked file is a conflict"     ro install adopt
check  "--overwrite adopts untracked file" ro --overwrite install adopt
check  "adopted file is the package's"    sh -c "[ \"\$('$RO/usr/bin/adopted')\" = packaged ]"
has    "adopted file is owned"            "owned by adopt" "$FPM_BIN" --root "$RO" --config "$T/fpm.conf" owner /usr/bin/adopted
check  "remove adopt"                     ro remove adopt

echo "usrmerge & purge"
RM=$T/merged-root
mkdir -p "$RM/usr/bin" "$RM/usr/lib"
ln -s bin "$RM/usr/sbin"; ln -s usr/bin "$RM/sbin"; ln -s usr/lib "$RM/lib64"
cp "$T/fpm.conf" "$T/merged.conf"
printf 'usrmerge = yes\npurge = usr/share/info/dir *.la\n' >> "$T/merged.conf"
mg() { "$FPM_BIN" --root "$RM" --config "$T/merged.conf" "$@" >/dev/null 2>&1; }
check  "install merged"                   mg install merged
check  "usr/sbin file is in usr/bin"      grep -q '^f - usr/bin/merged-a$' "$RM/var/lib/fpm/installed/merged/files"
check  "sbin file is in usr/bin"          grep -q '^f - usr/bin/merged-b$' "$RM/var/lib/fpm/installed/merged/files"
check  "lib64 file is in usr/lib"         grep -q '^f - usr/lib/merged.dat$' "$RM/var/lib/fpm/installed/merged/files"
check  "no usr/sbin entry in the package" sh -c "! grep -q 'usr/sbin\|^. - sbin\|lib64' '$RM/var/lib/fpm/installed/merged/files'"
check  "symlinked layout left intact"     sh -c "[ -L '$RM/usr/sbin' ] && [ -L '$RM/sbin' ] && [ -L '$RM/lib64' ]"
check  "program runs via /usr/sbin"       sh -c "[ \"\$('$RM/usr/sbin/merged-a')\" = usr-sbin ]"
has    "owner follows the /usr/sbin symlink" "owned by merged" "$FPM_BIN" --root "$RM" --config "$T/merged.conf" owner /usr/sbin/merged-a
has    "owner follows the /sbin symlink"   "owned by merged" "$FPM_BIN" --root "$RM" --config "$T/merged.conf" owner /sbin/merged-b
check  "info dir purged"                  test ! -e "$RM/usr/share/info/dir"
check  "*.la purged anywhere"             test ! -e "$RM/usr/lib/libmerged.la"
check  "other files kept"                 test -f "$RM/usr/share/info/merged.info"

echo "options = !usrmerge"
RL=$T/layout-root
mkdir -p "$RL"
check  "a package may ship /bin as a symlink" "$FPM_BIN" --root "$RL" --config "$T/merged.conf" install layout
check  "and it is installed"                 test -L "$RL/bin"
check  "a new directory gets the package's mode despite the umask" \
    sh -c "[ \"\$(stat -c %a '$RL/tmp')\" = 1777 ]"

echo "libraries without a soname"
RN=$T/nosoname-root
mkdir -p "$RN"
check  "program needing its own soname-less library installs" "$FPM_BIN" --root "$RN" --config "$T/fpm.conf" install nosoname
has    "the library's file name is provided" "libnoso.so" "$FPM_BIN" --root "$RN" --config "$T/fpm.conf" info nosoname

echo "options = !shlibs"
RB=$T/bundled-root
mkdir -p "$RB"
check  "missing libraries are not required"  "$FPM_BIN" --root "$RB" --config "$T/fpm.conf" install bundled
refuse "and none are recorded"               sh -c "\"$FPM_BIN\" --root '$RB' --config '$T/fpm.conf' info bundled | grep -q libbundled"

echo "libraries from the same transaction"
RT=$T/same-txn-root
mkdir -p "$RT"
check  "program and its library in one command" "$FPM_BIN" --root "$RT" --config "$T/fpm.conf" install hiprog hi
check  "both installed"                     test -x "$RT/usr/bin/hiprog" -a -f "$RT/usr/lib/libhi.so.1"
RT2=$T/same-txn-root2
mkdir -p "$RT2"
has    "still refused when nothing provides it" "needs libhi.so.1, which no installed package provides" "$FPM_BIN" --root "$RT2" --config "$T/fpm.conf" install hiprog

echo "a library two packages provide"
RY=$T/twin-root
mkdir -p "$RY"
y() { "$FPM_BIN" --root "$RY" --config "$T/fpm.conf" "$@"; }
check  "install both providers and a user"  y install twin-a twin-b twinuser
bump twin-b 2
check  "one may drop it while the other has it" y upgrade

echo "library cycles through a binary repository"
CB=$T/cyc-builder CS=$T/srv/cyc CR=$T/cyc-client
mkdir -p "$CB" "$CS" "$CR"
check  "build gccish and binutilsish"       "$FPM_BIN" --root "$CB" --config "$T/fpm.conf" build gccish binutilsish
cp "$CB"/var/cache/fpm/pkgs/*ish-*.fpm.tar.zst "$CS/"
check  "index them"                         "$FPM_BIN" index "$CS"
cat > "$T/cyc.conf" <<CONF
shlib_ignore = libc.so.6
[recipes none]
path = $EMPTY
[repo cyc]
url = file://$CS
allow_unsigned = yes
CONF
z() { "$FPM_BIN" --root "$CR" --config "$T/cyc.conf" "$@"; }
check  "sync"                               z sync
check  "install the pair despite the cycle" z -y install gccish
check  "both installed"                     test -f "$CR/usr/lib/libgx.so.1" -a -x "$CR/usr/bin/binutilsish"

echo "32-bit libraries"
if printf 'int main(void) { return 0; }\n' | cc -m32 -x c - -o "$T/m32" 2>/dev/null; then
    RW=$T/m32-root
    mkdir -p "$RW"
    w() { "$FPM_BIN" --root "$RW" --config "$T/fpm.conf" "$@"; }
    check  "64-bit libhi installs"              w install hi
    has    "32-bit program needs the 32-bit libhi" "needs libhi.so.1:32" w install hi32
    refuse "so it isn't installed"              test -e "$RW/usr/bin/hi32"
    check  "with lib32-hi it installs"          w install lib32-hi hi32
    has    "lib32-hi provides the tagged name"  "libhi.so.1:32" w info lib32-hi
    has    "hi still provides the plain name"   "libhi.so.1$" w info hi
else
    echo "  skip  (no 32-bit C compiler)"
fi

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
