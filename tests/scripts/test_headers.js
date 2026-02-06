// Test: Request/response headers
var resp = await fetch('http://localhost:18080/headers', {
    headers: {
        'X-Custom-Header': 'hello123',
        'Accept': 'application/json'
    }
});
if (resp.status !== 200) throw new Error('Expected 200, got ' + resp.status);
var data = await resp.json();
if (data['X-Custom-Header'] !== 'hello123')
    throw new Error('Server did not receive X-Custom-Header');
if (data['Accept'] !== 'application/json')
    throw new Error('Server did not receive Accept header');
console.log('PASS: test_headers');
