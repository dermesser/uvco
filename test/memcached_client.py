import asyncio
import aiomcache
import time

async def main():
    client = aiomcache.Client("::1", 9999)
    before = time.monotonic_ns()
    await client.set(b'key', b'value')

    for i in range(10000):
        await client.set(f'key{i}'.encode(), f'value{i}'.encode())
        await client.get(f'key{i}'.encode())
    after = time.monotonic_ns()
    print(f"Time taken: {(after-before)/1e9} s")

asyncio.run(main())

