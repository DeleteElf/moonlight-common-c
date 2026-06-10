## moonlight-common-c   (基于super系列配套)

### 主要功能说明
ps:仅相对于官方的moonlight-common-c说明，截止2026年06月10日
1. 从原版中去掉了所有enet相关的组件和api，并重新建立了网络通讯组件体系，新的网络通讯体系支持channel技术，不再需要多个端口。
2. 修改代理支持体系为网路通讯组件体系，完善相关功能，支持quic通讯组件、代理通讯组件。
3. quic通讯组件参考https://github.com/DeleteElf/network-quic
4. 全面修改视频解包器相关逻辑，现在支持多个显示器。
5. 完善rtp数据包，现在支持ssrc。
6. 编解码本身不再提供加密功能，是否加密由外部的网络通讯组件决定。
7. 新增基于http的rtsp协议支持，允许通过http callback实现rtsp协议走http协议。

