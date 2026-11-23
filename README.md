# FindFRs

FindFRs identifies frequent regions (FRs) in CuttleFish path data.

## Requirements

- C++17 compiler
- CMake 3.16 or newer
- Intel oneAPI Threading Building Blocks (TBB)

On macOS with Homebrew:

```bash
brew install cmake tbb
```

On Ubuntu/Debian:

```bash
sudo apt install cmake g++ libtbb-dev
```

## Build

```bash
cmake -S . -B build
cmake --build build --parallel
```

On macOS with Homebrew, if CMake does not locate TBB:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix tbb)"
cmake --build build --parallel
```

## Run

```bash
./build/findfrs3 <kappa> <alpha> <min_support> <min_avg_length> <cuttlefish_prefix>
```

Arguments:

- `kappa`: maximum distance between supporting occurrences
- `alpha`: minimum fraction of cluster nodes required for support
- `min_support`: minimum number of supporting paths
- `min_avg_length`: minimum average supporting-path length
- `cuttlefish_prefix`: prefix for the input `.cf_seg` and `.cf_seq` files

The output is written to a BED-like file named:

```text
<cuttlefish_prefix>.ka<kappa>.a<alpha>.ms<min_support>.ml<min_avg_length>.bed
```

Each output line contains:

```text
path_name  sequence_start  sequence_end  fr_id:variant_id
```

## Example

The `example/` directory contains a small valid CuttleFish data pair:

```bash
./build/findfrs3 1 0.75 1 1 example/sample
```

This creates `example/sample.ka1.a0.75.ms1.ml1.bed`.

## Input format

`<prefix>.cf_seg` contains one node and label per line:

```text
1 ACGT
2 CGTA
```

`<prefix>.cf_seq` contains a path name followed by node IDs:

```text
path1 1 2
path2 1 2
```
