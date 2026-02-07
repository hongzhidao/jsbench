// Test: Does Promise.all(fetch, fetch, fetch) run concurrently or sequentially?
//
// Each /delay/300 takes 300ms on the server side.
// If concurrent: total ~300ms
// If sequential: total ~900ms (3 * 300ms)

var N = 3;
var DELAY_MS = 300;
var urls = [];
for (var i = 0; i < N; i++) {
    urls.push('http://localhost:18080/delay/' + DELAY_MS);
}

var t0 = Date.now();

// Launch all fetches "concurrently" via Promise.all
var responses = await Promise.all(urls.map(url => fetch(url)));

var elapsed = Date.now() - t0;

// Verify all succeeded
for (var i = 0; i < responses.length; i++) {
    if (responses[i].status !== 200)
        throw new Error('Response ' + i + ' status: ' + responses[i].status);
}

console.log('Promise.all with ' + N + ' x ' + DELAY_MS + 'ms delay:');
console.log('  Total elapsed: ' + elapsed + 'ms');
console.log('  Expected if concurrent: ~' + DELAY_MS + 'ms');
console.log('  Expected if sequential: ~' + (N * DELAY_MS) + 'ms');

if (elapsed >= (N * DELAY_MS) * 0.8) {
    console.log('  RESULT: SEQUENTIAL (fetch is blocking, no concurrency)');
} else if (elapsed < DELAY_MS * 2) {
    console.log('  RESULT: CONCURRENT (fetches ran in parallel)');
} else {
    console.log('  RESULT: UNCLEAR (' + elapsed + 'ms)');
}

// Also test sequential for comparison
var t1 = Date.now();
for (var i = 0; i < N; i++) {
    var r = await fetch('http://localhost:18080/delay/' + DELAY_MS);
    if (r.status !== 200) throw new Error('Sequential fetch ' + i + ' failed');
}
var elapsed2 = Date.now() - t1;

console.log('');
console.log('Sequential await with ' + N + ' x ' + DELAY_MS + 'ms delay:');
console.log('  Total elapsed: ' + elapsed2 + 'ms');

console.log('');
if (Math.abs(elapsed - elapsed2) < DELAY_MS * 0.5) {
    console.log('CONCLUSION: Promise.all and sequential await take the same time.');
    console.log('  fetch() is BLOCKING - no real concurrency in CLI mode.');
} else {
    console.log('CONCLUSION: Promise.all is faster than sequential.');
    console.log('  fetch() supports real concurrency.');
}

console.log('PASS: test_fetch_concurrency');
