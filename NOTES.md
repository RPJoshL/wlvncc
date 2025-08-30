## Bauen und kopieren

```
ninja -C build && cp ./build/wlvncc /media/ubuntugui/videos/wlvncc
```

### Auf NUC ausführen

```
export TLS_CA="/home/ubuntugui/.config/wayvnc/tls_cert.pem"
export VNC_USERNAME="ubuntugui"
export VNC_PASSWORD="$(secret-tool lookup vnc arch)"

/media/ubuntugui/videos/wlvncc --hide-cursor --tls-cert "$TLS_CA" 192.168.1.15 5900 --app-id "vnc-arch"
```