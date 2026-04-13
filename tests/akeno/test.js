const { uws, app, label, generic_test, http_test, runTestsInOrder, paint, EXPECT_MATCH, WRITE_VALUE, makeRequest } = require("./misc/tester");
const http = require("http");
const stream = require('stream');

const FULL_MODE = process.argv.includes("--full");

// -- Begin tests --

label("Testing parser");

let parser;
generic_test("HTMLParser constructor", (ctx) => {
    parser = new uws.HTMLParser({
        buffer: true // Enable output
    });

    ctx.logPass();
});

generic_test("HTMLParser fromMarkdownString", (ctx) => {
    const result = parser.fromMarkdownString("# Hello World\n", parser.createContext());
    if (!result.toString().includes("<h1>Hello World</h1>")) {
        throw new Error("Markdown parsing failed");
    }

    ctx.logPass({ summary: result.toString() });
});

// HTML parsing is too volatile to test exactly
generic_test("HTMLParser fromFile", (ctx) => {
    const result = parser.fromFile(__dirname + "/misc/test.html", parser.createContext());
    if (!result.toString().includes("<!DOCTYPE html>")) {
        throw new Error("HTML file parsing failed");
    }

    ctx.logPass({ summary: result.toString().slice(0, 100).replaceAll("\n", "").replaceAll("\r", "") + "..." });
});

label("Testing routing");
http_test(`$id.localhost # Direct response`, WRITE_VALUE, EXPECT_MATCH);
http_test(`$id.localhost # Write in chunks`,
    (v) => (r, q) => { q.write(v.slice(0, 5)); q.write(v.slice(5)); q.end() },
    EXPECT_MATCH);

// =>> THE FOLLOWING TESTS FAIL DUE TO "CONNECTION: CLOSE", WHICH SHOULDN'T REALLY HAPPEN TO EVERYTHING OTHER THAN THE 404 HANDLER
http_test(`random # 404 response`, null, (res) => res.status === 404);
http_test(`*.localhost ($id.localhost, $id.localhost, !nope.$id.localhost) # Wildcard with multiple real hosts`, WRITE_VALUE, EXPECT_MATCH);
http_test(`test.*.localhost (test.$id.localhost, !$id.nope.localhost) # Wildcard in the middle`, WRITE_VALUE, EXPECT_MATCH);
http_test(`exact.localhost (exact.localhost, !no.match.localhost) # Exact host only`, WRITE_VALUE, EXPECT_MATCH);
http_test(`*.deep.noshallow (one.deep.noshallow, two.deep.noshallow, !deep.noshallow) # Deep wildcard only`, WRITE_VALUE, EXPECT_MATCH);
http_test(`alpha.*.* (alpha.$id.$id, !beta.$id.$id) # Multi wildcard`, WRITE_VALUE, EXPECT_MATCH);
http_test(`*.*.* ($id.$id.any, !$id.nope, !a.b.c.$id) # Match tripple wildcard`, WRITE_VALUE, EXPECT_MATCH);
http_test(`**.test_before (a.b.c.d.test_before, a.test_before, test_before, !a.b.c.d.e.no, !no.com, !something_else) # Anything before`, WRITE_VALUE, EXPECT_MATCH);
http_test(`test_after.** (test_after.a.b.c.d, test_after.a, test_after, !a.b.c.d.test_after, !no.com, !something_else) # Anything after`, WRITE_VALUE, EXPECT_MATCH);
// <==

http_test(`** (any.host.at.all) # Match all`, WRITE_VALUE, EXPECT_MATCH);


label("Testing serving capabilities");
let file = new uws.HTMLParser({ buffer: true }).fromFile(__dirname + "/misc/test.html", {});
if (Array.isArray(file)) {
    file = file.join("");
}
http_test(`$id.localhost # Serving parsed HTML file`, WRITE_VALUE, file);

http_test(`$id.localhost # Serving file as a stream`, (v) => (r, q) => {
    q.streamFile(__dirname + "/misc/test.html");
}, (res) => res.status === 200);

let large = new Array(10000).fill("Hello world! This is a particularly large file used in testing. It has no other meaning. ".repeat(10)).join("\n");
http_test(`$id.localhost # Serving large file as a copied string (~9MB)`, WRITE_VALUE, large);

large = Buffer.from(large);
http_test(`$id.localhost # Serving large file as a buffer (~9MB)`, WRITE_VALUE, large);
large = null;

// Test streaming a large file (1GB)
// Note: This is not an optimal way to stream a file since it ignores backpressure
if(false) http_test(`$id.localhost # Streaming huge file (~1GB)`, (v) => (r, q) => {
    const chunkSize = 1024 * 1024; // 1MB chunks
    const totalSize = 1024 * 1024 * 1024; // 1GB
    let sent = 0;

    const readable = new stream.Readable({
        read() {
            if (sent >= totalSize) {
                this.push(null);
                return;
            }

            const chunk = Buffer.alloc(Math.min(chunkSize, totalSize - sent), 'x');
            sent += chunk.length;
            this.push(chunk);
        }
    });

    q.onAborted(() => {
        readable.destroy();
        console.log(paint("yellow", "Request was aborted, stopping stream"));
    });

    readable.on('data', (chunk) => {
        q.write(chunk);
    });

    readable.on('end', () => {
        q.end();
    });
}, (res) => res.status === 200);

const testWebApp = new uws.WebApp(__dirname + "/misc");

app.route("test.localhost", testWebApp);

app.registerFileProcessor((id, url, path) => {
    console.log(paint("blue", `File processor called for ${url} (path: ${path})`), id);
    
    // Simulate async processing
    setTimeout(() => {
        app.completeProcessing(id, "<h1>Processed file content</h1>");
    }, 100);
})

http_test(`$id.localhost # Asynchronous response`, (v) => (r, q) => {
    let aborted = false;
    q.onAborted(() => {
        // If the request is aborted, we should not attempt to write to it
        console.log(paint("yellow", "Request was aborted, not writing response"));
        aborted = true;
    });

    setTimeout(() => {
        if(!aborted) q.cork(() => q.end(v));
    }, 100);
}, EXPECT_MATCH);

label("Testing websocket capabilities");

const WS_ECHO_PATH = "/akeno-test/ws-echo";
const WS_ROUTE_PATH = "/akeno-test/ws-route";
const WS_SCOPE_A_PATH = "/akeno-test/ws-scope-a";
const WS_SCOPE_B_PATH = "/akeno-test/ws-scope-b";
const WS_AUTH_PATH = "/akeno-test/ws-auth";

function toText(data) {
    if (typeof data === "string") {
        return Promise.resolve(data);
    }

    if (Buffer.isBuffer(data)) {
        return Promise.resolve(data.toString());
    }

    if (data instanceof ArrayBuffer) {
        return Promise.resolve(Buffer.from(data).toString());
    }

    if (ArrayBuffer.isView(data)) {
        return Promise.resolve(Buffer.from(data.buffer, data.byteOffset, data.byteLength).toString());
    }

    if (typeof Blob !== "undefined" && data instanceof Blob) {
        return data.text();
    }

    return Promise.resolve(String(data));
}

function waitForWebSocketOpen(ws, timeoutMs = 2000) {
    return new Promise((resolve, reject) => {
        const timeout = setTimeout(() => {
            cleanup();
            reject(new Error("WebSocket open timeout"));
        }, timeoutMs);

        const onOpen = () => {
            cleanup();
            resolve();
        };

        const onError = (event) => {
            cleanup();
            reject(new Error(`WebSocket error while opening: ${event?.message || "unknown error"}`));
        };

        const cleanup = () => {
            clearTimeout(timeout);
            ws.removeEventListener("open", onOpen);
            ws.removeEventListener("error", onError);
        };

        ws.addEventListener("open", onOpen, { once: true });
        ws.addEventListener("error", onError, { once: true });
    });
}

function waitForWebSocketMessage(ws, timeoutMs = 2000) {
    return new Promise((resolve, reject) => {
        const timeout = setTimeout(() => {
            cleanup();
            reject(new Error("WebSocket message timeout"));
        }, timeoutMs);

        const onMessage = (event) => {
            cleanup();
            resolve(event.data);
        };

        const onClose = () => {
            cleanup();
            reject(new Error("WebSocket closed before expected message"));
        };

        const cleanup = () => {
            clearTimeout(timeout);
            ws.removeEventListener("message", onMessage);
            ws.removeEventListener("close", onClose);
        };

        ws.addEventListener("message", onMessage, { once: true });
        ws.addEventListener("close", onClose, { once: true });
    });
}

function waitForWebSocketClose(ws, timeoutMs = 2000) {
    return new Promise((resolve, reject) => {
        const timeout = setTimeout(() => {
            cleanup();
            reject(new Error("WebSocket close timeout"));
        }, timeoutMs);

        const onClose = () => {
            cleanup();
            resolve();
        };

        const cleanup = () => {
            clearTimeout(timeout);
            ws.removeEventListener("close", onClose);
        };

        ws.addEventListener("close", onClose, { once: true });
    });
}

async function expectNoWebSocketMessage(ws, timeoutMs = 350) {
    try {
        await waitForWebSocketMessage(ws, timeoutMs);
        return false;
    } catch (error) {
        if (error && String(error.message).includes("timeout")) {
            return true;
        }
        throw error;
    }
}

function percentile(sortedValues, fraction) {
    if (!sortedValues.length) {
        return 0;
    }

    const index = Math.min(sortedValues.length - 1, Math.max(0, Math.floor((sortedValues.length - 1) * fraction)));
    return sortedValues[index];
}

async function runBenchmark(name, iterations, requestFactory) {
    const warmupIterations = Math.min(6, Math.max(0, Math.floor(iterations / 4)));

    for (let i = 0; i < warmupIterations; i++) {
        await requestFactory(i, true);
    }

    const samples = [];
    const startedAt = process.hrtime.bigint();

    for (let i = 0; i < iterations; i++) {
        const result = await requestFactory(i, false);
        samples.push(result.durationMs || 0);
    }

    const totalMs = Number(process.hrtime.bigint() - startedAt) / 1e6;
    const sorted = [...samples].sort((a, b) => a - b);
    const totalRequestMs = samples.reduce((sum, value) => sum + value, 0);
    const averageMs = totalRequestMs / samples.length;
    const reqPerSec = (samples.length / totalMs) * 1000;

    return `${name}: ${iterations} requests in ${totalMs.toFixed(1)}ms | ${reqPerSec.toFixed(1)} req/s | avg ${averageMs.toFixed(2)}ms | p50 ${percentile(sorted, 0.5).toFixed(2)}ms | p95 ${percentile(sorted, 0.95).toFixed(2)}ms | max ${sorted[sorted.length - 1].toFixed(2)}ms`;
}

app.ws(WS_ECHO_PATH, {
    message(ws, message, isBinary) {
        ws.send(message, isBinary);
    }
});

app.route(WS_ROUTE_PATH, {
    ws: {
        message(ws, message) {
            ws.send(`route:${Buffer.from(message).toString()}`);
        }
    }
});

app.ws(WS_SCOPE_A_PATH, {
    open(ws) {
        ws.subscribe("room");
    },
    message(ws, message) {
        if (Buffer.from(message).toString() === "emit") {
            ws.publish("room", "from-a");
        }
    }
});

app.ws(WS_SCOPE_B_PATH, {
    open(ws) {
        ws.subscribe("room");
    },
    message(ws, message) {
        if (Buffer.from(message).toString() === "emit") {
            ws.publish("room", "from-b");
        }
    }
});

app.ws(WS_AUTH_PATH, new uws.AuthenticatedWebSocket({
    authenticate(token) {
        if (String(token) === "valid-token") {
            return { userId: 1337 };
        }
        return null;
    },
    allowAuthHeader: false,
    allowFirstMessageAuth: true,
    sendErrors: true,
    open(ws) {
        ws.send(`AUTH_OK:${ws.user.userId}`);
    },
    message(ws, message) {
        ws.send(`ECHO:${Buffer.from(message).toString()}`);
    }
}));

const UPLOAD_PATH = "/akeno-test/upload";

app.route("upload.localhost", (req, res) => {
    const method = req.getMethod();
    const url = req.getUrl();
    const contentType = req.getHeader("content-type");
    const chunks = [];
    let aborted = false;

    res.onAborted(() => {
        aborted = true;
    });

    res.onData((chunk, isLast) => {
        chunks.push(Buffer.from(chunk));

        if (!isLast || aborted) {
            return;
        }

        const body = Buffer.concat(chunks);

        if (contentType && contentType.startsWith("multipart/form-data")) {
            const parts = uws.getParts(body, contentType) || [];
            const summary = parts.map((part) => {
                const name = part.name || "";
                const filename = part.filename ? `:${part.filename}` : "";
                const value = Buffer.from(part.data).toString("utf8").replaceAll("\n", "\\n").replaceAll("\r", "\\r");
                return `${name}${filename}=${value}`;
            }).join("|");

            res.end(`UPLOAD:MULTIPART:${method}:${url}:${parts.length}:${summary}`);
            return;
        }

        res.end(`UPLOAD:${method}:${url}:${body.length}:${body.toString("utf8")}`);
    });
});

generic_test("App.ws echo route", async (ctx) => {
    const ws = new WebSocket(`ws://127.0.0.1:8089${WS_ECHO_PATH}`);

    await waitForWebSocketOpen(ws);
    ws.send("hello-websocket");

    const data = await waitForWebSocketMessage(ws);
    const text = await toText(data);
    if (text !== "hello-websocket") {
        throw new Error(`Unexpected ws echo payload: ${text}`);
    }

    ws.close();
    await waitForWebSocketClose(ws);
    ctx.logPass({ summary: text });
});

generic_test("app.route(..., { ws }) websocket route", async (ctx) => {
    const ws = new WebSocket(`ws://127.0.0.1:8089${WS_ROUTE_PATH}`);

    await waitForWebSocketOpen(ws);
    ws.send("hello-route");

    const data = await waitForWebSocketMessage(ws);
    const text = await toText(data);
    if (text !== "route:hello-route") {
        throw new Error(`Unexpected route ws payload: ${text}`);
    }

    ws.close();
    await waitForWebSocketClose(ws);
    ctx.logPass({ summary: text });
});

generic_test("Scoped websocket topics are isolated per route", async (ctx) => {
    const wsAPublisher = new WebSocket(`ws://127.0.0.1:8089${WS_SCOPE_A_PATH}`);
    const wsASubscriber = new WebSocket(`ws://127.0.0.1:8089${WS_SCOPE_A_PATH}`);
    const wsBPublisher = new WebSocket(`ws://127.0.0.1:8089${WS_SCOPE_B_PATH}`);
    const wsBSubscriber = new WebSocket(`ws://127.0.0.1:8089${WS_SCOPE_B_PATH}`);

    await Promise.all([
        waitForWebSocketOpen(wsAPublisher),
        waitForWebSocketOpen(wsASubscriber),
        waitForWebSocketOpen(wsBPublisher),
        waitForWebSocketOpen(wsBSubscriber)
    ]);

    wsAPublisher.send("emit");
    const firstA = await toText(await waitForWebSocketMessage(wsASubscriber));
    if (firstA !== "from-a") {
        throw new Error(`Expected from-a for route A subscriber, got ${firstA}`);
    }

    if (!await expectNoWebSocketMessage(wsBSubscriber)) {
        throw new Error("Route B subscriber unexpectedly received route A publish");
    }

    wsBPublisher.send("emit");
    const firstB = await toText(await waitForWebSocketMessage(wsBSubscriber));
    if (firstB !== "from-b") {
        throw new Error(`Expected from-b for route B subscriber, got ${firstB}`);
    }

    if (!await expectNoWebSocketMessage(wsASubscriber)) {
        throw new Error("Route A subscriber unexpectedly received route B publish");
    }

    wsAPublisher.close();
    wsASubscriber.close();
    wsBPublisher.close();
    wsBSubscriber.close();
    await Promise.all([
        waitForWebSocketClose(wsAPublisher),
        waitForWebSocketClose(wsASubscriber),
        waitForWebSocketClose(wsBPublisher),
        waitForWebSocketClose(wsBSubscriber)
    ]);

    ctx.logPass({ summary: "isolated publish confirmed" });
});

generic_test("AuthenticatedWebSocket first-message auth", async (ctx) => {
    const ws = new WebSocket(`ws://127.0.0.1:8089${WS_AUTH_PATH}`);

    await waitForWebSocketOpen(ws);
    ws.send("valid-token");

    const authMessage = await toText(await waitForWebSocketMessage(ws));
    if (authMessage !== "AUTH_OK:1337") {
        throw new Error(`Unexpected auth message: ${authMessage}`);
    }

    ws.send("after-auth");
    const payload = await toText(await waitForWebSocketMessage(ws));
    if (payload !== "ECHO:after-auth") {
        throw new Error(`Unexpected authenticated payload: ${payload}`);
    }

    ws.close();
    await waitForWebSocketClose(ws);
    ctx.logPass({ summary: payload });
});

if (FULL_MODE) {
    label("Full mode checks");

    generic_test("POST upload echoes body", async (ctx) => {
        const response = await makeRequest("http", 8089, "upload.localhost", {
            method: "POST",
            path: UPLOAD_PATH,
            headers: {
                "Content-Type": "text/plain; charset=utf-8"
            },
            body: "hello upload"
        });

        const expected = `UPLOAD:post:${UPLOAD_PATH}:12:hello upload`;
        if (response.text !== expected) {
            throw new Error(`Unexpected upload response: ${response.text}`);
        }

        ctx.logPass({ summary: response.text });
    });

    generic_test("POST upload handles chunked body", async (ctx) => {
        const chunkedPayload = [Buffer.from("chunk-one-"), Buffer.from("chunk-two")];
        const chunkedLength = chunkedPayload[0].length + chunkedPayload[1].length;

        const response = await makeRequest("http", 8089, "upload.localhost", {
            method: "POST",
            path: UPLOAD_PATH,
            headers: {
                "Content-Type": "text/plain; charset=utf-8",
                "Content-Length": String(chunkedLength)
            },
            body: chunkedPayload
        });

        const expected = `UPLOAD:post:${UPLOAD_PATH}:19:chunk-one-chunk-two`;
        if (response.text !== expected) {
            throw new Error(`Unexpected chunked upload response: ${response.text}`);
        }

        ctx.logPass({ summary: response.text });
    });

    generic_test("POST upload parses multipart form-data", async (ctx) => {
        const boundary = "----akeno-boundary-123456";
        const body = Buffer.from([
            `--${boundary}\r\n`,
            'Content-Disposition: form-data; name="field"\r\n\r\n',
            'value\r\n',
            `--${boundary}\r\n`,
            'Content-Disposition: form-data; name="file"; filename="note.txt"\r\n',
            'Content-Type: text/plain\r\n\r\n',
            'file-body\r\n',
            `--${boundary}--\r\n`
        ].join(""));

        const response = await makeRequest("http", 8089, "upload.localhost", {
            method: "POST",
            path: UPLOAD_PATH,
            headers: {
                "Content-Type": `multipart/form-data; boundary=${boundary}`
            },
            body
        });

        const expected = `UPLOAD:MULTIPART:post:${UPLOAD_PATH}:2:field=value|file:note.txt=file-body`;
        if (response.text !== expected) {
            throw new Error(`Unexpected multipart upload response: ${response.text}`);
        }

        ctx.logPass({ summary: response.text });
    });

    generic_test("POST abort cleans up request", async (ctx) => {
        await new Promise((resolve, reject) => {
            let settled = false;

            const settle = (fn) => {
                if (settled) {
                    return;
                }

                settled = true;
                fn();
            };

            const req = http.request({
                hostname: "127.0.0.1",
                port: 8089,
                path: UPLOAD_PATH,
                method: "POST",
                headers: {
                    Host: "upload.localhost",
                    "Content-Type": "text/plain",
                    "Content-Length": 32
                }
            }, () => {
                settle(() => reject(new Error("Aborted upload unexpectedly received a response")));
            });

            req.on("error", () => settle(resolve));
            req.on("close", () => settle(resolve));
            req.write("partial-body");
            req.destroy();
        });

        await new Promise((resolve) => setTimeout(resolve, 50));
        ctx.logPass({ summary: "client abort handled" });
    });

    generic_test("WebSocket open/close cleanup loop", async (ctx) => {
        for (let i = 0; i < 8; i++) {
            const ws = new WebSocket(`ws://127.0.0.1:8089${WS_ECHO_PATH}`);

            await waitForWebSocketOpen(ws);
            ws.send(`round-${i}`);

            const data = await waitForWebSocketMessage(ws);
            const text = await toText(data);
            if (text !== `round-${i}`) {
                throw new Error(`Unexpected websocket echo during cleanup loop: ${text}`);
            }

            ws.close();
            await waitForWebSocketClose(ws);
        }

        ctx.logPass({ summary: "8 websocket cycles completed" });
    });

    generic_test("Benchmark GET direct response", async (ctx) => {
        const summary = await runBenchmark("GET / benchmark", 32, async () => {
            return makeRequest("http", 8089, "benchmark.localhost", {
                method: "GET",
                path: "/"
            });
        });

        ctx.logPass({ summary });
    });

    generic_test("Benchmark POST upload", async (ctx) => {
        const payload = Buffer.from("benchmark-payload-".repeat(64));
        const summary = await runBenchmark("POST /akeno-test/upload benchmark", 24, async () => {
            return makeRequest("http", 8089, "upload.localhost", {
                method: "POST",
                path: UPLOAD_PATH,
                headers: {
                    "Content-Type": "text/plain; charset=utf-8"
                },
                body: payload
            });
        });

        ctx.logPass({ summary });
    });
}

// This is currently REALLY VERY A LOT MUCH TOTALLY 100% broken
// label("Testing inline buffers as response");
// http_test(`buffer.localhost # Buffer response`,
//    Buffer.from("Hello world").buffer, "Hello world");

// label("Testing DeclarativeResponse");

runTestsInOrder().then(() => {
    console.log(paint("green", "All tests passed!"));
    if(!process.argv.includes("--keep-alive")) process.exit(0);
}).catch((err) => {
    console.error(paint("red", "Some tests failed."));
    if (err && err.message) {
        console.error(paint("red", err.message));
    }
    // Keep the process alive for debugging
});