/**
 * This is a testing utility that just does whatever
 */

const uws = require("../../dist/uws");
const http = require("http");
const https = require("https");

const EXPECT_MATCH = Symbol("EXPECT_MATCH");

const colors = {
    reset: "\x1b[0m",
    bold: "\x1b[1m",
    red: "\x1b[31m",
    green: "\x1b[32m",
    yellow: "\x1b[33m",
    blue: "\x1b[34m",
    magenta: "\x1b[35m",
    cyan: "\x1b[36m",
    gray: "\x1b[90m"
};

function paint(color, text) {
    return `${colors[color]}${text}${colors.reset}`;
}

function cleanText(text) {
    return String(text || "").replaceAll("\n", "").replaceAll("\r", "");
}

function summarizeResponse(res) {
    const text = cleanText(res.text);
    const snippet = text.slice(0, 100);
    return `${res.status} ${snippet}${text.length > 100 ? "..." : ""}`;
}

function logPass(info) {
    const status = paint("green", "PASS");
    const proto = paint("cyan", info.protocol.toUpperCase());
    const label = info.blocked ? paint("yellow", "BLOCKED") : "MATCH";
    const pattern = paint("gray", info.hostPattern);
    const summary = paint("gray", info.summary);
    console.log(`${status} ${proto} ${label} ${info.comment} ${pattern} (${info.host}) => ${summary}`);
}

function logFail(err, baseContext) {
    const ctx = Object.assign({}, baseContext, err && err.context ? err.context : {});
    const header = `${paint("red", "FAIL")} ${paint("cyan", (ctx.protocol || "?").toUpperCase())} ${ctx.comment || "(no comment)"}`;
    console.error(header);
    if (ctx.hostPattern || ctx.host) {
        console.error(`  ${paint("magenta", "Route")}: ${ctx.hostPattern || "?"} (${ctx.host || "?"})`);
    }
    if (ctx.expectedDesc) {
        console.error(`  ${paint("blue", "Expected")}: ${ctx.expectedDesc}`);
    }
    if (ctx.actualDesc) {
        console.error(`  ${paint("yellow", "Actual")}: ${ctx.actualDesc}`);
    }
    if (ctx.testString) {
        console.error(`  ${paint("gray", "Source")}: ${ctx.testString}`);
    }
    if(currentLabel) {
        console.error(`  ${paint("gray", "Label")}: ${currentLabel}`);
    }
    console.error(`  ${paint("red", "Reason")}: ${err && err.message ? err.message : err}`);
}

const app = new uws.App();

// Just testing that we can safely extend HTTPProtocol (Akeno currently relies on this for legacy JS APIs)
// class Extends extends uws.HTTPProtocol {
//     constructor() {
//         super();
//         console.log("Extending works");
//     }
// }

const p = 8089, p2 = 8090;
const httpProtocol = new uws.HTTPProtocol();
const httpsProtocol = new uws.HTTPSProtocol({
    key_file_name: "/www/dev/cert/key.pem",
    cert_file_name: "/www/dev/cert/cert.pem"
});

httpProtocol.listen(p, (socket) => console.log(socket)).bind(app);
httpsProtocol.listen(p2, (socket) => console.log(socket)).bind(app);

let tspmo = [];

// Helper to make HTTP/HTTPS requests with custom Host header
function makeRequest(protocol, port, host) {
    return new Promise((resolve, reject) => {
        const client = protocol === 'https' ? https : http;
        const options = {
            hostname: '127.0.0.1',
            port: port,
            path: '/',
            method: 'GET',
            headers: {
                'Host': host
            },
            rejectUnauthorized: false // For self-signed certs
        };

        const req = client.request(options, (res) => {
            let data = '';
            res.on('data', (chunk) => data += chunk);
            res.on('end', () => resolve({ status: res.statusCode, text: data }));
        });

        req.on('error', reject);
        req.end();
    });
}

let currentLabel = "";
function label(text) {
    tspmo.push(() => {
        console.log(paint("blue", `\n\n--- ${text} ---`));
        currentLabel = text;
    });
}

function generic_test(name, fn) {
    return tspmo.push(async () => {
        const baseContext = { testString: name, comment: name, hostPattern: name };
        try {
            await fn({
                paint,
                cleanText,
                summarizeResponse,
                logPass: (info) => logPass(Object.assign({
                    protocol: "GEN",
                    comment: name,
                    hostPattern: name,
                    host: "-",
                    blocked: false,
                    summary: "ok"
                }, typeof info === "string" ? { summary: info } : info)),
                logFail: (err) => logFail(err, baseContext)
            });
        } catch (err) {
            logFail(err, baseContext);
            throw err;
        }
    });
}

// Test utility function
function http_test(testString, handler, expected) {
    return tspmo.push(async () => {
        const [hostPart, commentPart] = testString.replaceAll("$id", () => id).split("#").map((s) => s.trim());
        const { hostPattern, realHosts, blockedHosts } = parseHostPart(hostPart);
        const comment = commentPart || hostPattern;
        const baseContext = { testString, comment, hostPattern };
        const expectedValue = expected === EXPECT_MATCH
            ? `MATCH ${comment} [${hostPattern}]`
            : expected;

        app.route(hostPattern, typeof handler === "function" ? handler(expectedValue) : handler);

        try {
            // Test against both HTTP and HTTPS
            for (const protocol of ['http', 'https']) {
                const port = protocol === 'http' ? p : p2;
                
                for (const realHost of realHosts) {
                    const res = await makeRequest(protocol, port, realHost);
                    const context = {
                        protocol,
                        host: realHost,
                        expectedDesc: typeof expectedValue === "function"
                            ? "custom assertion"
                            : `body == "${expectedValue}"${expected === EXPECT_MATCH ? " (EXPECT_MATCH)" : ""}`
                    };

                    if (typeof expectedValue === "function") {
                        try {
                            expectedValue(res, res.text, realHost, hostPattern);
                        } catch (err) {
                            err.context = Object.assign({}, context, {
                                actualDesc: summarizeResponse(res)
                            });
                            throw err;
                        }
                    } else if (res.text !== expectedValue) {
                        const err = new Error("Response body mismatch");
                        err.context = Object.assign({}, context, {
                            actualDesc: `body == "${cleanText(res.text)}"`
                        });
                        throw err;
                    }

                    logPass({
                        protocol,
                        comment,
                        hostPattern,
                        host: realHost,
                        blocked: false,
                        summary: summarizeResponse(res)
                    });
                }

                for (const blockedHost of blockedHosts) {
                    const res = await makeRequest(protocol, port, blockedHost);

                    if (res.status !== 404) {
                        const err = new Error("Blocked host matched unexpectedly");
                        err.context = {
                            protocol,
                            host: blockedHost,
                            expectedDesc: "status == 404 (blocked)",
                            actualDesc: `status == ${res.status}`
                        };
                        throw err;
                    }

                    logPass({
                        protocol,
                        comment,
                        hostPattern,
                        host: blockedHost,
                        blocked: true,
                        summary: `status ${res.status}`
                    });
                }
            }

        } catch (err) {
            logFail(err, baseContext);
            throw err;
        }
    });
}

async function runTestsInOrder() {
    for (const runTest of tspmo) {
        await runTest();
    }
}

function parseHostPart(hostPart) {
    const trimmed = hostPart.trim();
    const parenIndex = trimmed.indexOf("(");

    if (parenIndex === -1 || !trimmed.endsWith(")")) {
        return { hostPattern: trimmed, realHosts: [trimmed], blockedHosts: [] };
    }

    const hostPattern = trimmed.slice(0, parenIndex).trim();
    const inner = trimmed.slice(parenIndex + 1, -1);
    const rawHosts = inner
        .split(",")
        .map((host) => host.trim())
        .filter(Boolean);

    const blockedHosts = rawHosts
        .filter((host) => host.startsWith("!"))
        .map((host) => host.slice(1).trim())
        .filter(Boolean);

    const realHosts = rawHosts
        .filter((host) => !host.startsWith("!"));

    return {
        hostPattern,
        realHosts: realHosts.length ? realHosts : [hostPattern],
        blockedHosts
    };
}

let ic = 0;
Object.defineProperty(globalThis, "id", {
    get() {
        return ic++;
    }
});

module.exports = {
    label,
    generic_test,
    http_test,
    runTestsInOrder,
    paint,
    EXPECT_MATCH,
    uws
};