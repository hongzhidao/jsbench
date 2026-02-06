// Test: POST with JSON body
var resp = await fetch('http://localhost:18080/echo', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ name: 'test', value: 42 })
});
if (resp.status !== 200) throw new Error('Expected 200, got ' + resp.status);
var text = await resp.text();
var parsed = JSON.parse(text);
if (parsed.name !== 'test') throw new Error('Expected name "test", got "' + parsed.name + '"');
if (parsed.value !== 42) throw new Error('Expected value 42, got ' + parsed.value);
console.log('PASS: test_post');
