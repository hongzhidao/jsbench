export const bench = {
    connections: 50,
    duration: '10s',
    target: 'http://localhost:3000'
};

export default async function() {
    const res = await fetch('http://localhost:3000/auth/login', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ user: 'admin', pass: 'secret' })
    });
    const { token } = await res.json();

    return await fetch('http://localhost:3000/api/profile', {
        headers: { 'Authorization': `Bearer ${token}` }
    });
}
