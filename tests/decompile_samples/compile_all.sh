#!/usr/bin/env bash
# Compile one representative sample per language into /tmp/retdec_tests/
OUTDIR=/tmp/retdec_tests
SRCDIR="$(dirname "$(realpath "$0")")"

OK=0; FAIL=0
pass(){ echo "[ok] $1"; OK=$(( OK + 1 )); }
fail(){ echo "[FAIL] $1: $2"; FAIL=$(( FAIL + 1 )); }

########################################
# Java
########################################
compile_java(){
  cat > /tmp/retdec_tests/java/Fib.java <<'EOF'
public class Fib {
    public static long fibonacci(int n) {
        if (n <= 1) return n;
        long a = 0, b = 1;
        for (int i = 2; i <= n; i++) {
            long c = a + b; a = b; b = c;
        }
        return b;
    }
    public static void main(String[] args) {
        for (int i = 0; i < 10; i++)
            System.out.println("fib(" + i + ")=" + fibonacci(i));
    }
}
EOF
  # Target Java 8 class format so d8 (Android build tool) can process it
  javac --release 8 /tmp/retdec_tests/java/Fib.java -d /tmp/retdec_tests/java/ 2>&1
}
compile_java && pass "Java (Fib.class)" || fail "Java" "$?"

########################################
# DEX (Java -> .class -> .dex)
########################################
compile_dex(){
  ~/android-sdk/build-tools/34.0.0/d8 \
    /tmp/retdec_tests/java/Fib.class \
    --output /tmp/retdec_tests/dex/ 2>&1
}
compile_dex && pass "DEX (classes.dex)" || fail "DEX" "$?"

########################################
# Kotlin
########################################
compile_kotlin(){
  which kotlinc >/dev/null 2>&1 || { echo "kotlinc not in PATH"; return 1; }
  cat > /tmp/retdec_tests/kotlin/Hello.kt <<'EOF'
fun factorial(n: Long): Long = if (n <= 1) 1 else n * factorial(n - 1)
fun main() {
    (1L..10L).forEach { println("$it! = ${factorial(it)}") }
}
EOF
  kotlinc /tmp/retdec_tests/kotlin/Hello.kt -include-runtime \
    -d /tmp/retdec_tests/kotlin/Hello.jar 2>&1
}
compile_kotlin && pass "Kotlin (Hello.jar)" || fail "Kotlin" "kotlinc not available"

########################################
# C# (.NET)
########################################
compile_csharp(){
  mkdir -p /tmp/retdec_tests/csharp/BubbleSort
  cat > /tmp/retdec_tests/csharp/BubbleSort/Program.cs <<'EOF'
using System;
class BubbleSort {
    static void Sort(int[] a) {
        for (int i = 0; i < a.Length - 1; i++)
            for (int j = 0; j < a.Length - 1 - i; j++)
                if (a[j] > a[j + 1]) { int t = a[j]; a[j] = a[j+1]; a[j+1] = t; }
    }
    static void Main() {
        int[] arr = {64, 34, 25, 12, 22, 11, 90};
        Sort(arr);
        Console.WriteLine(string.Join(", ", arr));
    }
}
EOF
  cat > /tmp/retdec_tests/csharp/BubbleSort/BubbleSort.csproj <<'EOF'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
  </PropertyGroup>
</Project>
EOF
  dotnet publish /tmp/retdec_tests/csharp/BubbleSort/BubbleSort.csproj \
    -c Release -o /tmp/retdec_tests/csharp/publish/ --nologo 2>&1
}
compile_csharp && pass "C# (BubbleSort)" || fail "C#" "$?"

########################################
# Python pyc
########################################
compile_python(){
  cat > /tmp/retdec_tests/python/primes.py <<'EOF'
def sieve(limit):
    is_prime = [True] * (limit + 1)
    is_prime[0] = is_prime[1] = False
    for i in range(2, int(limit**0.5) + 1):
        if is_prime[i]:
            for j in range(i*i, limit+1, i):
                is_prime[j] = False
    return [i for i, p in enumerate(is_prime) if p]

if __name__ == "__main__":
    print(sieve(50))
EOF
  python3 -m py_compile /tmp/retdec_tests/python/primes.py
  # Move .pyc to known path
  pyc=$(python3 -c "import py_compile, importlib.util; \
    py_compile.compile('/tmp/retdec_tests/python/primes.py', \
    cfile='/tmp/retdec_tests/python/primes.pyc')")
  # Try direct path
  python3 -c "import py_compile; py_compile.compile('/tmp/retdec_tests/python/primes.py', cfile='/tmp/retdec_tests/python/primes.pyc')"
}
compile_python && pass "Python (primes.pyc)" || fail "Python" "$?"

########################################
# Lua bytecode
########################################
compile_lua(){
  cat > /tmp/retdec_tests/lua/mergesort.lua <<'EOF'
local function merge(a, lo, mid, hi)
    local tmp = {}
    local i, j = lo, mid + 1
    while i <= mid and j <= hi do
        if a[i] <= a[j] then tmp[#tmp+1] = a[i]; i = i + 1
        else tmp[#tmp+1] = a[j]; j = j + 1 end
    end
    while i <= mid do tmp[#tmp+1] = a[i]; i = i + 1 end
    while j <= hi  do tmp[#tmp+1] = a[j]; j = j + 1 end
    for k, v in ipairs(tmp) do a[lo + k - 1] = v end
end
local function mergesort(a, lo, hi)
    if lo < hi then
        local mid = math.floor((lo + hi) / 2)
        mergesort(a, lo, mid); mergesort(a, mid+1, hi); merge(a, lo, mid, hi)
    end
end
local t = {5,3,8,1,9,2,7,4,6}
mergesort(t, 1, #t)
print(table.concat(t, ", "))
EOF
  luac -o /tmp/retdec_tests/lua/mergesort.luac /tmp/retdec_tests/lua/mergesort.lua 2>&1
}
compile_lua && pass "Lua (mergesort.luac)" || fail "Lua" "$?"

########################################
# WebAssembly
########################################
compile_wasm(){
  cat > /tmp/retdec_tests/wasm/math.wat <<'EOF'
(module
  (func $add (export "add") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.add)
  (func $mul (export "mul") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.mul)
  (func $gcd (export "gcd") (param i32 i32) (result i32)
    (local $tmp i32)
    block $exit
      loop $loop
        local.get 1
        i32.eqz
        br_if $exit
        local.get 0
        local.get 1
        i32.rem_u
        local.set $tmp
        local.get 1
        local.set 0
        local.get $tmp
        local.set 1
        br $loop
      end
    end
    local.get 0)
)
EOF
  wat2wasm /tmp/retdec_tests/wasm/math.wat -o /tmp/retdec_tests/wasm/math.wasm 2>&1
}
compile_wasm && pass "WebAssembly (math.wasm)" || fail "WASM" "$?"

########################################
# C (stripped ELF)
########################################
compile_c(){
  cat > /tmp/retdec_tests/c/sort.c <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*cmp_fn)(const void*, const void*);

static int cmp_int(const void* a, const void* b) {
    return *(const int*)a - *(const int*)b;
}

static int binary_search(const int* arr, int n, int target) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (arr[mid] == target) return mid;
        if (arr[mid] < target)  lo = mid + 1;
        else                    hi = mid - 1;
    }
    return -1;
}

int main(void) {
    int data[] = {42, 7, 19, 3, 100, 55, 28};
    int n = sizeof(data)/sizeof(data[0]);
    qsort(data, n, sizeof(int), cmp_int);
    for (int i = 0; i < n; i++) printf("%d ", data[i]);
    printf("\nfound 28 at idx %d\n", binary_search(data, n, 28));
    return 0;
}
EOF
  gcc -O2 -o /tmp/retdec_tests/c/sort /tmp/retdec_tests/c/sort.c
  strip /tmp/retdec_tests/c/sort
}
compile_c && pass "C (sort, stripped)" || fail "C" "$?"

########################################
# C++
########################################
compile_cpp(){
  cat > /tmp/retdec_tests/cpp/graph.cpp <<'EOF'
#include <iostream>
#include <vector>
#include <queue>
#include <unordered_map>

class Graph {
    std::unordered_map<int, std::vector<int>> adj;
public:
    void add_edge(int u, int v) { adj[u].push_back(v); adj[v].push_back(u); }
    std::vector<int> bfs(int start) {
        std::vector<int> visited;
        std::unordered_map<int,bool> seen;
        std::queue<int> q;
        q.push(start); seen[start] = true;
        while (!q.empty()) {
            int v = q.front(); q.pop();
            visited.push_back(v);
            for (int nb : adj[v]) if (!seen[nb]) { seen[nb]=true; q.push(nb); }
        }
        return visited;
    }
};

int main() {
    Graph g;
    g.add_edge(1,2); g.add_edge(1,3); g.add_edge(2,4); g.add_edge(3,5);
    for (int v : g.bfs(1)) std::cout << v << " ";
    std::cout << "\n";
    return 0;
}
EOF
  g++ -O2 -std=c++17 -o /tmp/retdec_tests/cpp/graph /tmp/retdec_tests/cpp/graph.cpp
  strip /tmp/retdec_tests/cpp/graph
}
compile_cpp && pass "C++ (graph BFS, stripped)" || fail "C++" "$?"

########################################
# Rust
########################################
compile_rust(){
  cat > /tmp/retdec_tests/rust/primes.rs <<'EOF'
fn sieve(limit: usize) -> Vec<usize> {
    let mut is_prime = vec![true; limit + 1];
    is_prime[0] = false;
    if limit > 0 { is_prime[1] = false; }
    let mut i = 2;
    while i * i <= limit {
        if is_prime[i] { let mut j = i*i; while j <= limit { is_prime[j] = false; j += i; } }
        i += 1;
    }
    is_prime.iter().enumerate().filter(|(_, &p)| p).map(|(i, _)| i).collect()
}
fn main() {
    println!("{:?}", sieve(50));
}
EOF
  rustc -O -o /tmp/retdec_tests/rust/primes /tmp/retdec_tests/rust/primes.rs 2>&1
  strip /tmp/retdec_tests/rust/primes
}
compile_rust && pass "Rust (primes, stripped)" || fail "Rust" "$?"

########################################
# Go
########################################
compile_go(){
  cat > /tmp/retdec_tests/go/matrix.go <<'EOF'
package main

import "fmt"

type Matrix [3][3]float64

func (m Matrix) Mul(b Matrix) Matrix {
    var r Matrix
    for i := 0; i < 3; i++ {
        for j := 0; j < 3; j++ {
            for k := 0; k < 3; k++ {
                r[i][j] += m[i][k] * b[k][j]
            }
        }
    }
    return r
}

func main() {
    a := Matrix{{1,2,3},{4,5,6},{7,8,9}}
    b := Matrix{{9,8,7},{6,5,4},{3,2,1}}
    c := a.Mul(b)
    for _, row := range c { fmt.Println(row) }
}
EOF
  go build -o /tmp/retdec_tests/go/matrix /tmp/retdec_tests/go/matrix.go
  strip /tmp/retdec_tests/go/matrix
}
compile_go && pass "Go (matrix, stripped)" || fail "Go" "$?"

########################################
# x86-64 NASM assembly
########################################
compile_asm(){
  cat > /tmp/retdec_tests/asm/strlen.asm <<'EOF'
section .data
    msg db "Hello from NASM!", 0
section .text
global _start
_start:
    ; strlen
    mov rdi, msg
    xor rcx, rcx
.loop:
    cmp byte [rdi+rcx], 0
    je  .done
    inc rcx
    jmp .loop
.done:
    ; write(1, msg, rcx)
    mov rax, 1
    mov rdi, 1
    mov rsi, msg
    mov rdx, rcx
    syscall
    ; write newline
    mov rax, 1
    mov rdi, 1
    push 10
    mov rsi, rsp
    mov rdx, 1
    syscall
    pop rdi
    ; exit(0)
    xor rdi, rdi
    mov rax, 60
    syscall
EOF
  nasm -f elf64 /tmp/retdec_tests/asm/strlen.asm -o /tmp/retdec_tests/asm/strlen.o
  ld /tmp/retdec_tests/asm/strlen.o -o /tmp/retdec_tests/asm/strlen
}
compile_asm && pass "x86-64 ASM (strlen)" || fail "ASM" "$?"

########################################
# Summary
########################################
echo ""
echo "=============================="
echo "Compiled: $OK   Failed: $FAIL"
echo "=============================="
ls -lh /tmp/retdec_tests/java/Fib.class \
        /tmp/retdec_tests/dex/classes.dex \
        /tmp/retdec_tests/python/primes.pyc \
        /tmp/retdec_tests/lua/mergesort.luac \
        /tmp/retdec_tests/wasm/math.wasm \
        /tmp/retdec_tests/c/sort \
        /tmp/retdec_tests/cpp/graph \
        /tmp/retdec_tests/rust/primes \
        /tmp/retdec_tests/go/matrix \
        /tmp/retdec_tests/asm/strlen \
        2>/dev/null | awk '{print $5, $9}'
[ -f /tmp/retdec_tests/kotlin/Hello.jar ] && ls -lh /tmp/retdec_tests/kotlin/Hello.jar | awk '{print $5, $9}'
[ -f /tmp/retdec_tests/csharp/publish/BubbleSort ] && ls -lh /tmp/retdec_tests/csharp/publish/BubbleSort | awk '{print $5, $9}'
[ -f /tmp/retdec_tests/csharp/publish/BubbleSort.dll ] && ls -lh /tmp/retdec_tests/csharp/publish/BubbleSort.dll | awk '{print $5, $9}'
