var resp = await fetch('http://localhost:3000/api/health');
console.log('status:', resp.status);

var data = await resp.json();
console.log('body:', data);
