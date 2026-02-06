// ─── Benchmark config ───────────────────────────────────
export const bench = {
    connections: 10,        // concurrent connections
    threads:     2,          // worker threads
    duration:    '10s',      // duration (s/ms/m/h)
};

// ─── Request target ─────────────────────────────────────
//
// 1) Simple GET — a URL string:
export default 'http://localhost:8080/';
//
// 2) Custom request — an object:
// export default {
//     url:     'http://localhost:8080/api/users',
//     method:  'POST',
//     headers: { 'Content-Type': 'application/json' },
//     body:    JSON.stringify({ name: 'test' })
// };
//
// 3) Round-robin — an array:
// export default [
//     'http://localhost:8080/health',
//     'http://localhost:8080/api/status',
//     {
//         url:     'http://localhost:8080/api/data',
//         method:  'POST',
//         headers: { 'Content-Type': 'application/json' },
//         body:    JSON.stringify({ key: 'value' })
//     }
// ];
//
// 4) Custom scenario — an async function:
// export default async function() {
//     const res = await fetch('http://localhost:8080/api/login', {
//         method: 'POST',
//         headers: { 'Content-Type': 'application/json' },
//         body: JSON.stringify({ user: 'admin', pass: 'secret' })
//     });
//     const { token } = await res.json();
//     return await fetch('http://localhost:8080/api/data', {
//         headers: { 'Authorization': 'Bearer ' + token }
//     });
// }
