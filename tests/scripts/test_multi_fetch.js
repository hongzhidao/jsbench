// Test: Sequential fetches
var r1 = await fetch('http://localhost:18080/health');
var t1 = await r1.text();
if (t1 !== 'OK') throw new Error('First fetch failed');

var r2 = await fetch('http://localhost:18080/json');
var d2 = await r2.json();
if (d2.message !== 'hello') throw new Error('Second fetch failed');

var r3 = await fetch('http://localhost:18080/status/200');
if (r3.status !== 200) throw new Error('Third fetch failed');

console.log('PASS: test_multi_fetch');
