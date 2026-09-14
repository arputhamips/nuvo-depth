#!/usr/bin/env node

// Local development relay for viewing a TLS-only Nuvo appliance from a
// browser without trusting the appliance's temporary engineering certificate.
// It authenticates once, serves the repository web UI on loopback, proxies the
// control API, and tunnels WebSocket frames without decoding or copying their
// H.264 payloads in JavaScript.

const fs = require('fs');
const http = require('http');
const https = require('https');
const path = require('path');
const tls = require('tls');

function argument(name, fallback = '') {
  const index = process.argv.indexOf(name);
  return index >= 0 && index + 1 < process.argv.length
    ? process.argv[index + 1]
    : fallback;
}

const upstreamHost = argument('--upstream-host', 'cammow.local');
const upstreamPort = Number(argument('--upstream-port', '17443'));
const listenHost = argument('--listen-host', '127.0.0.1');
const listenPort = Number(argument('--listen-port', '8767'));
const webRoot = path.resolve(argument('--web-root', path.join(process.cwd(), 'web')));
const tokenPath = argument('--token-file');

if (!tokenPath || !Number.isInteger(upstreamPort) || !Number.isInteger(listenPort)) {
  throw new Error('usage: live_stream_relay.js --token-file PATH [--upstream-host HOST] [--upstream-port PORT] [--listen-port PORT]');
}
const claimToken = fs.readFileSync(tokenPath, 'utf8').trim();
if (!claimToken) throw new Error('claim token file is empty');
// The relay retains the short-lived value in memory; do not leave the copied
// appliance credential on the desktop filesystem after startup.
fs.rmSync(tokenPath, {force: true});

let upstreamCookie = '';
let upstreamSession = {};
let sessionRefresh;

function claimSession() {
  return new Promise((resolve, reject) => {
    const request = https.request({
      host: upstreamHost,
      port: upstreamPort,
      path: '/api/v1/session',
      method: 'POST',
      rejectUnauthorized: false,
      headers: {Authorization: `Bearer ${claimToken}`},
    }, response => {
      const chunks = [];
      response.on('data', chunk => chunks.push(chunk));
      response.on('end', () => {
        if (response.statusCode !== 200) {
          reject(new Error(`appliance claim failed with HTTP ${response.statusCode}`));
          return;
        }
        const cookies = response.headers['set-cookie'] || [];
        upstreamCookie = cookies.map(value => value.split(';', 1)[0]).join('; ');
        try {
          upstreamSession = JSON.parse(Buffer.concat(chunks).toString('utf8'));
        } catch (error) {
          reject(new Error(`appliance returned an invalid session: ${error.message}`));
          return;
        }
        if (!upstreamCookie || !upstreamSession.csrf) {
          reject(new Error('appliance session omitted its cookie or CSRF token'));
          return;
        }
        resolve();
      });
    });
    request.on('error', reject);
    request.end();
  });
}

function refreshSession() {
  if (!sessionRefresh) {
    sessionRefresh = claimSession().finally(() => { sessionRefresh = undefined; });
  }
  return sessionRefresh;
}

const contentTypes = new Map([
  ['.css', 'text/css; charset=utf-8'],
  ['.html', 'text/html; charset=utf-8'],
  ['.js', 'text/javascript; charset=utf-8'],
  ['.json', 'application/json; charset=utf-8'],
  ['.png', 'image/png'],
  ['.svg', 'image/svg+xml'],
]);

function localSession(response) {
  const body = Buffer.from(JSON.stringify(upstreamSession));
  response.writeHead(200, {
    'content-type': 'application/json; charset=utf-8',
    'content-length': body.length,
    'cache-control': 'no-store',
  });
  response.end(body);
}

async function proxyApi(request, response) {
  function fail(error) {
    if (!response.headersSent) {
      const body = Buffer.from(JSON.stringify({error: error.message}));
      response.writeHead(502, {'content-type': 'application/json', 'content-length': body.length});
      response.end(body);
    } else {
      response.destroy(error);
    }
  }
  try {
    const chunks = [];
    let length = 0;
    for await (const chunk of request) {
      length += chunk.length;
      if (length > 8 * 1024 * 1024) {
        response.writeHead(413).end('Request too large');
        return;
      }
      chunks.push(chunk);
    }
    const body = Buffer.concat(chunks, length);
    function send(retried) {
      const headers = {...request.headers};
      for (const name of ['host', 'connection', 'cookie', 'transfer-encoding',
                          'content-length']) delete headers[name];
      headers.host = `${upstreamHost}:${upstreamPort}`;
      headers.cookie = upstreamCookie;
      headers.origin = `https://${upstreamHost}:${upstreamPort}`;
      if (body.length) headers['content-length'] = body.length;
      if (!['GET', 'HEAD'].includes(request.method || 'GET')) {
        headers['x-nuvo-csrf'] = upstreamSession.csrf;
      }
      const upstream = https.request({
        host: upstreamHost,
        port: upstreamPort,
        path: request.url,
        method: request.method,
        headers,
        rejectUnauthorized: false,
      }, upstreamResponse => {
        if (upstreamResponse.statusCode === 401 && !retried) {
          upstreamResponse.resume();
          upstreamResponse.on('end', () => {
            refreshSession().then(() => send(true)).catch(fail);
          });
          return;
        }
        const outgoing = {...upstreamResponse.headers};
        delete outgoing['set-cookie'];
        response.writeHead(upstreamResponse.statusCode || 502, outgoing);
        upstreamResponse.pipe(response);
      });
      upstream.on('error', fail);
      upstream.end(body);
    }
    send(false);
  } catch (error) {
    fail(error);
  }
}

function serveStatic(request, response) {
  const pathname = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
  const relative = pathname === '/' ? 'index.html' : pathname.replace(/^\/+/, '');
  const file = path.resolve(webRoot, relative);
  if (file !== webRoot && !file.startsWith(`${webRoot}${path.sep}`)) {
    response.writeHead(403).end('Forbidden');
    return;
  }
  fs.readFile(file, (error, bytes) => {
    if (error) {
      response.writeHead(error.code === 'ENOENT' ? 404 : 500).end('Not found');
      return;
    }
    response.writeHead(200, {
      'content-type': contentTypes.get(path.extname(file)) || 'application/octet-stream',
      'content-length': bytes.length,
      'cache-control': 'no-store',
    });
    response.end(bytes);
  });
}

function tunnelWebSocket(request, client, clientHead, retried = false) {
  const allowed = new Set(['/api/v1/stream', '/api/v1/encoded/stream']);
  const target = new URL(request.url, 'http://localhost').pathname;
  if (!allowed.has(target)) {
    client.destroy();
    return;
  }
  const upstream = tls.connect({
    host: upstreamHost,
    port: upstreamPort,
    rejectUnauthorized: false,
  });
  upstream.on('secureConnect', () => {
    const headers = [
      `GET ${target} HTTP/1.1`,
      `Host: ${upstreamHost}:${upstreamPort}`,
      'Upgrade: websocket',
      'Connection: Upgrade',
      `Sec-WebSocket-Key: ${request.headers['sec-websocket-key']}`,
      `Sec-WebSocket-Version: ${request.headers['sec-websocket-version'] || '13'}`,
      `Cookie: ${upstreamCookie}`,
      `Origin: https://${upstreamHost}:${upstreamPort}`,
    ];
    for (const name of ['sec-websocket-protocol', 'sec-websocket-extensions']) {
      if (request.headers[name]) headers.push(`${name}: ${request.headers[name]}`);
    }
    upstream.write(`${headers.join('\r\n')}\r\n\r\n`);
  });

  let handshake = Buffer.alloc(0);
  const acceptHandshake = chunk => {
    handshake = Buffer.concat([handshake, chunk]);
    const end = handshake.indexOf('\r\n\r\n');
    if (end < 0) return;
    upstream.removeListener('data', acceptHandshake);
    const header = handshake.subarray(0, end + 4);
    if (!header.toString('latin1').startsWith('HTTP/1.1 101')) {
      if (!retried && header.toString('latin1').startsWith('HTTP/1.1 401')) {
        upstream.destroy();
        refreshSession().then(() => {
          if (!client.destroyed) tunnelWebSocket(request, client, clientHead, true);
        }).catch(() => client.destroy());
        return;
      }
      client.write(header);
      client.destroy();
      upstream.destroy();
      return;
    }
    client.write(header);
    const remainder = handshake.subarray(end + 4);
    if (remainder.length) client.write(remainder);
    if (clientHead.length) upstream.write(clientHead);
    client.pipe(upstream);
    upstream.pipe(client);
  };
  upstream.on('data', acceptHandshake);
  upstream.on('error', () => client.destroy());
  client.on('error', () => upstream.destroy());
}

claimSession().then(() => {
  const server = http.createServer((request, response) => {
    const pathname = new URL(request.url, 'http://localhost').pathname;
    if (pathname === '/api/v1/session') {
      localSession(response);
    } else if (pathname.startsWith('/api/v1/')) {
      proxyApi(request, response);
    } else {
      serveStatic(request, response);
    }
  });
  server.on('upgrade', tunnelWebSocket);
  server.listen(listenPort, listenHost, () => {
    process.stdout.write(`Nuvo live viewer: http://${listenHost}:${listenPort}/\n`);
  });
}).catch(error => {
  process.stderr.write(`Nuvo live viewer failed: ${error.message}\n`);
  process.exitCode = 1;
});
