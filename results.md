test: OK
## Results

`1,000,000 points · 20,000 zones · 2,000 queries per scenario · AMD Ryzen 5 5600GT with Radeon Graphics`

### Build: points

| Structure | build (ms) |
|---|---:|
| Naive scan | 0.0 |
| B-tree (lat) | 88.2 |
| Geohash | 92.7 |
| Quadtree | 257.0 |
| R-tree (STR) | 199.5 |

### Build: zones

| Structure | build (ms) |
|---|---:|
| Naive scan | 0.0 |
| Geohash | 2.6 |
| Quadtree | 1.4 |
| R-tree (STR) | 3.1 |

### 1. Radius search: 500 m ("drivers near me")

_avg 959.8 results/query_

| Structure | µs / query | queries / s | vs naive | correct |
|---|---:|---:|---:|:---:|
| Naive scan | 888.34 | 1,126 | 1.0× | ✅ |
| B-tree (lat) | 86.85 | 11,515 | 10.2× | ✅ |
| Geohash | 13.69 | 73,025 | 64.9× | ✅ |
| Quadtree | 13.23 | 75,608 | 67.2× | ✅ |
| **R-tree (STR)** | **11.60** | 86,183 | 76.6× | ✅ |

### 2. Radius search: 5 km ("restaurants in my area")

_avg 56068.2 results/query_

| Structure | µs / query | queries / s | vs naive | correct |
|---|---:|---:|---:|:---:|
| Naive scan | 1551.93 | 644 | 1.0× | ✅ |
| B-tree (lat) | 1033.85 | 967 | 1.5× | ✅ |
| Geohash | 655.80 | 1,525 | 2.4× | ✅ |
| Quadtree | 622.48 | 1,606 | 2.5× | ✅ |
| **R-tree (STR)** | **453.01** | 2,207 | 3.4× | ✅ |

### 3. k-nearest: k = 1 ("closest driver")

_avg 1.0 results/query_

| Structure | µs / query | queries / s | vs naive | correct |
|---|---:|---:|---:|:---:|
| Naive scan | 1295.27 | 772 | 1.0× | ✅ |
| B-tree (lat) | 52.81 | 18,936 | 24.5× | ✅ |
| Geohash | 13.39 | 74,683 | 96.7× | ✅ |
| **Quadtree** | **0.87** | 1,153,600 | 1494.2× | ✅ |
| R-tree (STR) | 1.95 | 513,326 | 664.9× | ✅ |

### 4. k-nearest: k = 20 ("20 closest stores")

_avg 20.0 results/query_

| Structure | µs / query | queries / s | vs naive | correct |
|---|---:|---:|---:|:---:|
| Naive scan | 1294.91 | 772 | 1.0× | ✅ |
| B-tree (lat) | 59.81 | 16,719 | 21.6× | ✅ |
| Geohash | 21.19 | 47,183 | 61.1× | ✅ |
| **Quadtree** | **3.34** | 299,749 | 388.1× | ✅ |
| R-tree (STR) | 4.55 | 219,827 | 284.7× | ✅ |

### 5. Point in polygon ("which delivery zones cover me?")

_avg 34.8 results/query_

| Structure | µs / query | queries / s | vs naive | correct |
|---|---:|---:|---:|:---:|
| Naive scan | 65.00 | 15,386 | 1.0× | ✅ |
| **Geohash** | **3.67** | 272,702 | 17.7× | ✅ |
| Quadtree | 9.49 | 105,427 | 6.9× | ✅ |
| R-tree (STR) | 4.49 | 222,511 | 14.5× | ✅ |

### 6. Polygon overlap ("which zones overlap this new zone?")

_avg 281.9 results/query_

| Structure | µs / query | queries / s | vs naive | correct |
|---|---:|---:|---:|:---:|
| Naive scan | 139.55 | 7,166 | 1.0× | ✅ |
| **Geohash** | **52.00** | 19,230 | 2.7× | ✅ |
| Quadtree | 56.57 | 17,676 | 2.5× | ✅ |
| R-tree (STR) | 61.12 | 16,361 | 2.3× | ✅ |

All structures match the naive scan. ✅
