#!/usr/bin/env python3
import sys, asyncio, argparse
from aiocoap import Context, Message, Code

# Windows event loop fix for asyncio + aiocoap
if sys.platform.startswith("win"):
    asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())

def build_uri(scheme: str, host: str, port: int, path: str) -> str:
    path = path.lstrip("/")
    if port and port != 5683:
        return f"{scheme}://{host}:{port}/{path}"
    return f"{scheme}://{host}/{path}"

async def one_request(method: str, uri: str, data: bytes | None, timeout: float):
    ctx = await Context.create_client_context()
    code = Code.GET if method.upper() == "GET" else Code.POST
    msg = Message(code=code, uri=uri, payload=(data or b""))
    try:
        resp = await asyncio.wait_for(ctx.request(msg).response, timeout=timeout)
        print(f"[OK] {method} {uri} -> {resp.code} | {resp.payload.decode('utf-8', 'ignore')}")
    except asyncio.TimeoutError:
        print(f"[TIMEOUT] {method} {uri} (>{timeout}s)")
    except Exception as e:
        print(f"[ERROR] {method} {uri}: {e!r}")

async def main():
    p = argparse.ArgumentParser(description="Minimal CoAP client (GET/POST).")
    p.add_argument("--host", default="127.0.0.1", help="Servidor (IP/DNS). Ej.: 127.0.0.1 o 34.235.x.x")
    p.add_argument("--port", type=int, default=5683, help="Puerto (por defecto 5683)")
    p.add_argument("--scheme", default="coap", choices=["coap"], help="Esquema (solo coap en este ejemplo)")
    p.add_argument("--method", default="GET", choices=["GET", "POST"], help="Método")
    p.add_argument("--path", default="sensor", help="Recurso. Ej.: sensor o echo")
    p.add_argument("--data", default="", help="Payload para POST (texto). Ignorado en GET")
    p.add_argument("--timeout", type=float, default=5.0, help="Timeout en segundos")
    args = p.parse_args()

    uri = build_uri(args.scheme, args.host, args.port, args.path)
    payload = args.data.encode("utf-8") if args.method.upper() == "POST" else None
    await one_request(args.method, uri, payload, args.timeout)

if __name__ == "__main__":
    asyncio.run(main())
