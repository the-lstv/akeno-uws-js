const { uws, label, generic_test, http_test, runTestsInOrder, paint, EXPECT_MATCH } = require("./tester");

// -- Begin tests --

label("Testing parser");

let parser;
generic_test("HTMLParser constructor", (ctx) => {
    parser = new uws.HTMLParser({
        buffer: true,
        onText: () => {},
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
    const result = parser.fromFile(__dirname + "/test.html", parser.createContext());
    if (!result.toString().includes("<!DOCTYPE html>")) {
        throw new Error("HTML file parsing failed");
    }

    ctx.logPass({ summary: result.toString().slice(0, 100).replaceAll("\n", "").replaceAll("\r", "") + "..." });
});

label("Testing routing");
http_test(`$id.localhost # Direct response`, (v) => ((r, q) => { q.end(v) }), EXPECT_MATCH);
http_test(`$id.localhost # Write in chunks`, (v) => ((r, q) => { q.write(v.slice(0, 5)); q.write(v.slice(5)); q.end() }), EXPECT_MATCH);
http_test(`random # 404 response`, null, (res) => res.status === 404);
http_test(`*.localhost ($id.localhost, $id.localhost, !nope.$id.localhost) # Wildcard with multiple real hosts`, (v) => ((r, q) => { q.end(v) }), EXPECT_MATCH);
http_test(`test.*.localhost (test.$id.localhost, !$id.nope.localhost) # Wildcard in the middle`, (v) => ((r, q) => { q.end(v) }), EXPECT_MATCH);
http_test(`exact.localhost (exact.localhost, !no.match.localhost) # Exact host only`, (v) => ((r, q) => { q.end(v) }), EXPECT_MATCH);
http_test(`*.deep.noshallow (one.deep.noshallow, two.deep.noshallow, !deep.noshallow) # Deep wildcard only`, (v) => ((r, q) => { q.end(v) }), EXPECT_MATCH);
http_test(`alpha.*.* (alpha.$id.$id, !beta.$id.$id) # Multi wildcard`, (v) => ((r, q) => { q.end(v) }), EXPECT_MATCH);
http_test(`*.*.* ($id.$id.any, !$id.nope, !a.b.c.$id) # Match tripple wildcard`, (v) => ((r, q) => { q.end(v) }), EXPECT_MATCH);
http_test(`**.test_before (a.b.c.d.test_before, a.test_before, test_before, !a.b.c.d.e.no, !no.com, !something_else) # Anything before`, (v) => ((r, q) => { q.end(v) }), EXPECT_MATCH);
http_test(`test_after.** (test_after.a.b.c.d, test_after.a, test_after, !a.b.c.d.test_after, !no.com, !something_else) # Anything after`, (v) => ((r, q) => { q.end(v) }), EXPECT_MATCH);
http_test(`** (any.host.at.all) # Match all`, (v) => ((r, q) => { q.end(v) }), EXPECT_MATCH);

// This is broken
// label("Testing inline buffers as response");
// test(`buffer.localhost # Buffer response`, Buffer.from("Hello world").buffer, "Hello world");

// label("Testing DeclarativeResponse");

runTestsInOrder().then(() => {
    console.log(paint("green", "All tests passed!"));
    process.exit(0);
}).catch((err) => {
    console.error(paint("red", "Some tests failed."));
    if (err && err.message) {
        console.error(paint("red", err.message));
    }
});