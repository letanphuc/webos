# Sudoku HTTPS WASM sample

This C WASM application calls the API Ninjas Sudoku endpoint through the
WebOS `web_http_request` host API and prints the JSON response.

Set the API key in the shell rather than storing it in source control:

```sh
export API_NINJAS_KEY="your-api-key"
wdb --serial /dev/tty.usbserial-1130 app run webos/sampleapps/sudoku -- "$API_NINJAS_KEY"
```

Pass a second argument to use another compatible URL. The default is:

```text
https://api.api-ninjas.com/v1/sudokugenerate?difficulty=easy
```
