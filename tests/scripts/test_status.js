// Test: Various HTTP status codes
var r200 = await fetch('http://localhost:18080/status/200');
if (r200.status !== 200) throw new Error('Expected 200, got ' + r200.status);
if (!r200.ok) throw new Error('Expected ok for 200');

var r404 = await fetch('http://localhost:18080/status/404');
if (r404.status !== 404) throw new Error('Expected 404, got ' + r404.status);
if (r404.ok) throw new Error('Expected !ok for 404');

var r500 = await fetch('http://localhost:18080/status/500');
if (r500.status !== 500) throw new Error('Expected 500, got ' + r500.status);
if (r500.ok) throw new Error('Expected !ok for 500');

console.log('PASS: test_status');
