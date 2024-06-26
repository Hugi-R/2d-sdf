import random

N_SEGMENTS = 1000

# Function to generate random segments
def generate_random_segments(num_segments=10):
    segments = []
    for _ in range(num_segments):
        x1, y1 = round(random.uniform(0, 1), 2), round(random.uniform(0, 1), 5)
        x2, y2 = round(random.uniform(0, 1), 2), round(random.uniform(0, 1), 5)
        round_value = round(random.uniform(0.0001, 0.01), 5)
        segment = f"ROUND(SEGMENT(POINT({x1} {y1}) POINT({x2} {y2})) {round_value})"
        segments.append(segment)
    return segments

# Generate and print 10 random segments
random_segments = generate_random_segments(N_SEGMENTS)
for segment in random_segments:
    print(segment)
