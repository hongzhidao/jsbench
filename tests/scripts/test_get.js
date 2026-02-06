// Test: Basic GET request, .status, .text()
var resp = await fetch('http://localhost:18080/health');
if (resp.status !== 200) throw new Error('Expected status 200, got ' + resp.status);
var text = await resp.text();
if (text !== 'OK') throw new Error('Expected body "OK", got "' + text + '"');
console.log('PASS: test_get');
