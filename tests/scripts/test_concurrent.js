// Test: Promise.all concurrent fetches
var urls = [
    'http://localhost:18080/health',
    'http://localhost:18080/json',
    'http://localhost:18080/status/200'
];

var responses = await Promise.all(urls.map(url => fetch(url)));

if (responses.length !== 3) throw new Error('Expected 3 responses');
for (var i = 0; i < responses.length; i++) {
    if (responses[i].status !== 200)
        throw new Error('Response ' + i + ' status: ' + responses[i].status);
}

var text = await responses[0].text();
if (text !== 'OK') throw new Error('health response wrong');

var json = await responses[1].json();
if (json.message !== 'hello') throw new Error('json response wrong');

console.log('PASS: test_concurrent');
