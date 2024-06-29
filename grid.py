STEP = 10

print("LAYER(1)")
for i in range(STEP, 100, STEP):
    for j in range(STEP, 100, STEP):
        x = round(i / 100, 3)
        y = round(j / 100, 3)
        print(f"ROUND(0.015 POINT({x} {y} COLOR({x} {y} {1-x} 1)))")

for i in range(0, 9*9, 1):
        if (i % (9+1)) == 0:
             print(f"ROUND(0.002 SEGMENT({i} {i+10}))")
             continue
        if ((i+1) % 9) != 0:
            # Link top
            print(f"ROUND(0.002 SEGMENT({i} {i+1}))")
        if i+9 < 9*9:
            # Link right
            print(f"ROUND(0.002 SEGMENT({i} {i+9}))")

