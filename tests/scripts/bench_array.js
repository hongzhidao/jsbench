// Test: Array round-robin benchmark
export const bench = {
    connections: 5,
    duration: '1s',
    threads: 1
};
export default [
    'http://localhost:18080/health',
    'http://localhost:18080/json'
];
