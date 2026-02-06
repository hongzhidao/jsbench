// Test: Async function default export benchmark
export const bench = {
    connections: 2,
    duration: '1s',
    threads: 1,
    target: 'http://localhost:18080'
};
export default async function() {
    return await fetch('http://localhost:18080/health');
}
