# Sudoku HTTPS Rust WASM sample

This `no_std` Rust WASM application calls the API Ninjas Sudoku endpoint through
the WebOS `web_http_request` host API, parses the 9x9 puzzle, solves it locally
with an iterative backtracking solver, and prints both grids. Empty cells in the
puzzle are displayed as `.`.

Set the API key in the shell rather than storing it in source control:

```sh
export API_NINJAS_KEY="your-api-key"
wdb --serial /dev/tty.usbserial-1130 app run webos/sampleapps/rust_sudoku -- "$API_NINJAS_KEY"
```

Pass a second argument to use another compatible URL. The default is:

```text
https://api.api-ninjas.com/v1/sudokugenerate?difficulty=easy
```
