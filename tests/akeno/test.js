const { uws, label, generic_test, http_test, runTestsInOrder, paint, EXPECT_MATCH, WRITE_VALUE } = require("./misc/tester");
const stream = require('stream');

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
http_test(`random # 404 response`, null, (res) => res.status === 404);
http_test(`*.localhost ($id.localhost, $id.localhost, !nope.$id.localhost) # Wildcard with multiple real hosts`, WRITE_VALUE, EXPECT_MATCH);
http_test(`test.*.localhost (test.$id.localhost, !$id.nope.localhost) # Wildcard in the middle`, WRITE_VALUE, EXPECT_MATCH);
http_test(`exact.localhost (exact.localhost, !no.match.localhost) # Exact host only`, WRITE_VALUE, EXPECT_MATCH);
http_test(`*.deep.noshallow (one.deep.noshallow, two.deep.noshallow, !deep.noshallow) # Deep wildcard only`, WRITE_VALUE, EXPECT_MATCH);
http_test(`alpha.*.* (alpha.$id.$id, !beta.$id.$id) # Multi wildcard`, WRITE_VALUE, EXPECT_MATCH);
http_test(`*.*.* ($id.$id.any, !$id.nope, !a.b.c.$id) # Match tripple wildcard`, WRITE_VALUE, EXPECT_MATCH);
http_test(`**.test_before (a.b.c.d.test_before, a.test_before, test_before, !a.b.c.d.e.no, !no.com, !something_else) # Anything before`, WRITE_VALUE, EXPECT_MATCH);
http_test(`test_after.** (test_after.a.b.c.d, test_after.a, test_after, !a.b.c.d.test_after, !no.com, !something_else) # Anything after`, WRITE_VALUE, EXPECT_MATCH);
http_test(`** (any.host.at.all) # Match all`, WRITE_VALUE, EXPECT_MATCH);


label("Testing serving capabilities");
const file = new uws.HTMLParser({ buffer: true }).fromFile(__dirname + "/misc/test.html", {});
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
http_test(`$id.localhost # Streaming huge file (~1GB)`, (v) => (r, q) => {
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

// This is currently broken
// label("Testing inline buffers as response");
// test(`buffer.localhost # Buffer response`,
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