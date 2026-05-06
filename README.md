# ngx_http_req_count_module

A custom NGINX HTTP module for tracking request counts for specific paths or location blocks directly inside NGINX.

---

# Problem Statement

NGINX provides the `stub_status` module which exposes general server statistics such as active connections and the total number of requests processed by the server. While this is useful for understanding overall traffic, it does not provide visibility into how many requests are being served by individual paths or location blocks.

In many environments, path-level request statistics are usually obtained by analysing NGINX access logs. This often involves building and maintaining a separate pipeline for collecting logs, parsing them, storing them in a database, and querying the stored data later for analytics.

Although this approach works well for large-scale observability systems, it can become unnecessary overhead when the requirement is simply to know how many requests are hitting certain endpoints or locations in NGINX.

This module provides a simpler alternative by maintaining request counters directly inside NGINX using shared memory. It allows request counts to be collected and served without depending on external logging or analytics infrastructure.

---

# Features

- Request counting per location block
- Shared memory based counters
- Lightweight runtime overhead
- Simple configuration
- No external log parsing required
- Metrics exposed directly from NGINX

---

# Build and Installation

## 1. Install Packages
### Ubuntu / Debian

Install the required packages:

```bash
sudo apt update

sudo apt install -y \
    git \
    build-essential \
    libpcre2-dev \
    zlib1g-dev
```

---

### RHEL / CentOS / Rocky Linux / AlmaLinux

Install the required packages:

```bash
sudo dnf install -y \
    git \
    gcc \
    gcc-c++ \
    make \
    pcre2-devel \
    zlib-devel
```

For older systems using `yum`:

```bash
sudo yum install -y \
    git \
    gcc \
    gcc-c++ \
    make \
    pcre2-devel \
    zlib-devel
```

---

## 2. Download NGINX Source

Download the same version of NGINX that is installed on your system.

Example:

```bash
wget https://nginx.org/download/nginx-1.26.0.tar.gz

tar -xzf nginx-1.26.0.tar.gz

cd nginx-1.26.0
```

---

## 3. Clone the Module Repository

Clone the repository to any directory of your choice.

Example:

```bash
git clone <YOUR_MODULE_REPO_URL>
```

This will create:

```text
ngx_http_req_count_module/
```

Replace `<YOUR_MODULE_REPO_URL>` with your repository URL.

---

## 4. Configure NGINX With the Module

Run the NGINX configure script and provide the module path using `--add-module`.

Example:

```bash
sudo ./configure \
    --add-module=/path/to/ngx_http_req_count_module
```

If your existing NGINX installation uses additional configure arguments, include them as well.

You can view the current configure arguments using:

```bash
nginx -V
```

Example:

```bash
sudo ./configure \
    --prefix=/etc/nginx \
    --sbin-path=/usr/sbin/nginx \
    --conf-path=/etc/nginx/nginx.conf \
    --pid-path=/var/run/nginx.pid \
    --lock-path=/var/run/nginx.lock \
    --error-log-path=/var/log/nginx/error.log \
    --http-log-path=/var/log/nginx/access.log \
    --add-module=/path/to/ngx_http_req_count_module
```

---

## 5. Build NGINX

```bash
make
```

---

## 6. Install

```bash
sudo make install
```

---

## 7. Verify Installation

Run:

```bash
nginx -V
```

You should see the module path in the configure arguments.

Example:

```text
--add-module=/path/to/ngx_http_req_count_module
```

---

# Configuration

Below is a simple example configuration.

```nginx
http {

    count_zone name=req_counter freq=10m;

    server {

        listen 80;

        location /api {

            count_req req_counter;

            proxy_pass http://backend;
        }

        location /metrics {

            count_get req_counter;
        }
    }
}
```

---

# Directives

## `count_zone`

Creates a shared memory zone for storing request counters.
The user provides name for the count_zone and also provides the frequency after which the counter should be recorded. The units for freq can be s(seconds), m(minutes) or h(hour).

Can be only defined in the `MAIN` config.

### Syntax

```nginx
count_zone zone=<name> freq=<value><unit>;
```

### Example

```nginx
count_zone zone=req_counter freq=30s;
```

---

## `count_req`

Enables request counting for any block. The user must provide the name of the `count_zone` here.

Can be defined in `MAIN`, `SERVER` or `LOCATION` blocks.
### Syntax

```nginx
req_count name=<zone_name>;
```

### Example

```nginx
location /api {
    count_req name=req_counter;
}
```

---

## `count_get`

Serves the current request counts through an HTTP endpoint. he user must provide the name of the count_zone here.

Can only be defined in `LOCATION` block.

### Syntax

```nginx
count_get name=<zone_name>;
```

### Example

```nginx
location /metrics {
    count_get name=req_counter;
}
```

---

# Example

## Configuration

```nginx
http {

    count_zone zone=my_zone freq=1m;

    server {

        listen 80;

        location /path1 {

            count_req name=my_zone;

            return 200 "path1\n";
        }

        location /path2 {

            count_req name=my_zone;

            return 200 "path2\n";
        }

        location /count {

            count_get name=my_zone;
        }
    }
}
```

---

## Generate Traffic

```bash
curl http://localhost/path1

curl http://localhost/path1

curl http://localhost/path2
```

---

## Fetch Request Counts

```bash
curl http://localhost/count
```

Example response:

```text
3
```

---

# How It Works

- A shared memory zone is created during configuration parsing.
- Request counters are stored inside shared memory.
- Requests hitting configured locations increment their counters.
- A dedicated handler exposes the current counter values through an endpoint.
- Since the counters are stored in shared memory, all worker processes share the same state.

---

# Notes

- Counters reset when NGINX restarts/reloads.
- The module is intended for lightweight request statistics and monitoring use cases.

---

# Future Improvements

Possible future enhancements include:

- Per-method request counts
- Response status code statistics
- JSON formatted responses