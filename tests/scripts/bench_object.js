// Test: Object default export benchmark (POST)
export const bench = {
    connections: 5,
    duration: '1s',
    threads: 1
};
export default {
    url: 'http://localhost:18080/echo',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ name: 'bench_test' })
};
