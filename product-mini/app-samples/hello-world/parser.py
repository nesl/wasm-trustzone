f = open("renju-cross-check.wasm","rb")
num_list = list(f.read())
results = []

for num in num_list:
    results.append(hex(num))

print(', '.join(results))
f.close()
