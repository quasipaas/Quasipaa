use std::{net::SocketAddr, sync::Arc, time::Instant};

use crate::{config::Config, monitor::Monitor};

use axum::{
    extract::{Path, State},
    http::StatusCode,
    response::IntoResponse,
    routing::{delete, get},
    Json, Router,
};

use serde_json::{json, Value};
use tokio::{
    net::TcpListener,
    sync::mpsc::{unbounded_channel, UnboundedSender},
};

use turn::Service;

struct AppState {
    config: Arc<Config>,
    service: Service,
    monitor: Monitor,
    uptime: Instant,
}

/// start http server
///
/// Create an http server and start it, and you can access the controller
/// instance through the http interface.
///
/// Warn: This http server does not contain
/// any means of authentication, and sensitive information and dangerous
/// operations can be obtained through this service, please do not expose it
/// directly to an unsafe environment.
pub async fn start_server(
    config: Arc<Config>,
    service: Service,
    monitor: Monitor,
) -> anyhow::Result<()> {
    let app = Router::new()
        .route(
            "/info",
            get(|State(state): State<Arc<AppState>>| async move {
                let router = state.service.get_router();
                Json(json!({
                    "software": concat!(
                        env!("CARGO_PKG_NAME"),
                        ":",
                        env!("CARGO_PKG_VERSION")
                    ),
                    "uptime": state.uptime.elapsed().as_secs(),
                    "realm": state.config.turn.realm,
                    "port_allocated": router.len(),
                    "port_capacity": router.capacity(),
                    "interfaces": state.config.turn.interfaces,
                }))
            }),
        )
        .route(
            "/session/:addr/info",
            get(
                |Path(addr): Path<SocketAddr>, State(state): State<Arc<AppState>>| async move {
                    if let (Some(node), Some(counts)) = (
                        state.service.get_router().get_node(&Arc::new(addr)),
                        state.monitor.get(&addr),
                    ) {
                        Json(json!({
                            "username": node.username,
                            "password": node.password,
                            "allocated_channels": node.channels,
                            "allocated_ports": node.ports,
                            "expiration": node.expiration,
                            "lifetime": node.lifetime.elapsed().as_secs(),
                            "received_bytes": counts.received_bytes,
                            "send_bytes": counts.send_bytes,
                            "received_pkts": counts.received_pkts,
                            "send_pkts": counts.send_pkts,
                        }))
                        .into_response()
                    } else {
                        StatusCode::NOT_FOUND.into_response()
                    }
                },
            ),
        )
        .route(
            "/session/:addr",
            delete(
                |Path(addr): Path<SocketAddr>, State(state): State<Arc<AppState>>| async move {
                    if state.service.get_router().remove(&Arc::new(addr)).is_some() {
                        StatusCode::OK
                    } else {
                        StatusCode::EXPECTATION_FAILED
                    }
                },
            ),
        )
        .with_state(Arc::new(AppState {
            config: config.clone(),
            uptime: Instant::now(),
            service,
            monitor,
        }));

    log::info!("controller server listening={:?}", &config.api.bind);
    axum::serve(TcpListener::bind(config.api.bind).await?, app).await?;

    Ok(())
}

pub struct HooksService {
    client: Arc<reqwest::Client>,
    tx: UnboundedSender<Value>,
    cfg: Arc<Config>,
}

impl HooksService {
    pub fn new(cfg: Arc<Config>) -> Self {
        let client = Arc::new(reqwest::Client::new());

        let cfg_ = cfg.clone();
        let client_ = client.clone();
        let (tx, mut rx) = unbounded_channel::<Value>();
        tokio::spawn(async move {
            if let Some(server) = &cfg_.api.hooks {
                let uri = format!("{}/events", server);

                while let Some(signal) = rx.recv().await {
                    if let Err(e) = client_.post(&uri).json(&signal).send().await {
                        log::error!("failed to request hooks server, err={:?}", e);
                    }
                }
            }
        });

        Self { client, cfg, tx }
    }

    pub async fn get_password(&self, addr: &SocketAddr, name: &str) -> Option<String> {
        if let Some(server) = &self.cfg.api.hooks {
            if let Ok(res) = self
                .client
                .get(format!("{}/password?addr={}&name={}", server, addr, name))
                .send()
                .await
            {
                if let Ok(password) = res.text().await {
                    return Some(password);
                }
            }
        }

        None
    }

    pub fn send_event(&self, event: Value) {
        if self.cfg.api.hooks.is_some() {
            let _ = self.tx.send(event);
        }
    }
}