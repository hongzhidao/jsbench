export const bench = {
    connections: 100,
    duration: '10s'
};

export default [
    'http://localhost:3000/health',
    {
        url: 'http://localhost:3000/api/users',
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name: 'test' })
    }
];
