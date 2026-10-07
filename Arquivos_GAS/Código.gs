const ABA_CARTOES = "Cartoes";
const ABA_ACESSOS = "Acessos";
const ABA_ESCANINHOS = "Escaninhos";
const ABA_DISPOSITIVOS = "Dispositivos";
const LIMITE_REGISTROS = 100;
const TEMPO_CADASTRO_MS = 2 * 60 * 1000;
const TEMPO_ONLINE_MS = 2 * 60 * 1000;
const TOKEN_INICIAL = "RFID-ESCANINHOS-2026-SEGURANCA-123456789";

function doGet() {
  return HtmlService.createHtmlOutputFromFile("index")
      .setTitle("Sistema de Escaninhos")
      .addMetaTag("viewport", "width=device-width, initial-scale=1");
}

function configurarSistema() {
  const planilha = SpreadsheetApp.getActiveSpreadsheet();
  const cartoes = garantirAba(planilha, ABA_CARTOES, ["UID", "Nome", "Escaninho", "Ativo", "CadastradoEm"]);
  const acessos = garantirAba(planilha, ABA_ACESSOS, ["DataHora", "UID", "Nome", "Escaninho", "Status", "Metodo", "Dispositivo"]);
  const escaninhos = garantirAba(planilha, ABA_ESCANINHOS, ["ID", "Nome", "Ativo", "PinHash", "PinSalt", "PinAtualizadoEm"]);
  const dispositivos = garantirAba(planilha, ABA_DISPOSITIVOS, ["ID", "Escaninho", "Nome", "UltimaComunicacao", "IP", "RSSI", "Firmware"]);
  aplicarCabecalho(cartoes, 5);
  aplicarCabecalho(acessos, 7);
  aplicarCabecalho(escaninhos, 6);
  aplicarCabecalho(dispositivos, 7);
  if (escaninhos.getLastRow() < 2) escaninhos.appendRow(["ESC-01", "Escaninho 01", "SIM", "", "", ""]);
  const propriedades = PropertiesService.getScriptProperties();
  propriedades.setProperty("API_TOKEN", TOKEN_INICIAL);
  if (!propriedades.getProperty("PIN_PEPPER"))
    propriedades.setProperty("PIN_PEPPER", Utilities.getUuid() + Utilities.getUuid() + Date.now());
  return "Sistema configurado.";
}

function garantirAba(planilha, nome, cabecalho) {
  let aba = planilha.getSheetByName(nome);
  if (!aba) aba = planilha.insertSheet(nome);
  aba.getRange(1, 1, 1, cabecalho.length).setValues([cabecalho]);
  aba.setFrozenRows(1);
  return aba;
}

function aplicarCabecalho(aba, quantidade) {
  aba.getRange(1, 1, 1, quantidade).setFontWeight("bold").setBackground("#101828").setFontColor("#ffffff");
  aba.autoResizeColumns(1, quantidade);
}

function doPost(e) {
  try {
    if (!e || !e.postData || !e.postData.contents)
      return responderJSON({sucesso: false, autorizado: false, mensagem: "Nenhum dado recebido"});
    const dados = JSON.parse(e.postData.contents);
    if (!tokenValido(dados.token)) return responderJSON({sucesso: false, autorizado: false, mensagem: "Token inválido"});
    const planilha = SpreadsheetApp.getActiveSpreadsheet();
    const abaCartoes = planilha.getSheetByName(ABA_CARTOES);
    const abaAcessos = planilha.getSheetByName(ABA_ACESSOS);
    const abaEscaninhos = planilha.getSheetByName(ABA_ESCANINHOS);
    const abaDispositivos = planilha.getSheetByName(ABA_DISPOSITIVOS);
    if (!abaCartoes || !abaAcessos || !abaEscaninhos || !abaDispositivos) throw new Error("Execute configurarSistema primeiro.");
    const acao = String(dados.acao || "").trim().toLowerCase();
    const escaninho = normalizarID(dados.escaninho);
    if (escaninho) {
      garantirEscaninhoRegistrado(abaEscaninhos, escaninho);
      atualizarDispositivo(abaDispositivos, dados);
    }
    if (acao === "heartbeat") return responderJSON({sucesso: true, autorizado: false, mensagem: "Heartbeat recebido"});
    if (acao === "rfid") return processarRFIDServidor(abaCartoes, abaAcessos, abaEscaninhos, dados);
    if (acao === "pin") return processarPINServidor(abaAcessos, abaEscaninhos, dados);
    return responderJSON({sucesso: false, autorizado: false, mensagem: "Ação inválida"});
  } catch (erro) {
    return responderJSON({sucesso: false, autorizado: false, mensagem: erro.message || erro.toString()});
  }
}

function tokenValido(recebido) {
  const correto = PropertiesService.getScriptProperties().getProperty("API_TOKEN");
  return (correto && String(recebido || "") === correto);
}

function processarRFIDServidor(abaCartoes, abaAcessos, abaEscaninhos, dados) {
  const uid = normalizarUID(dados.uid);
  const escaninho = normalizarID(dados.escaninho);
  const dispositivo = String(dados.dispositivo || "");
  if (!uid || !escaninho) return responderJSON({sucesso: false, autorizado: false, mensagem: "Dados inválidos"});
  const dadosEscaninho = buscarEscaninho(abaEscaninhos, escaninho);
  if (!dadosEscaninho || !dadosEscaninho.ativo) {
    registrarAcesso(abaAcessos, [new Date(), uid, "", escaninho, "NEGADO", "RFID", dispositivo]);
    return responderJSON({sucesso: true, autorizado: false, cadastrado: false, status: "NEGADO", mensagem: "Escaninho desativado"});
  }
  const cadastro = obterCadastroPendente(escaninho);
  if (cadastro) {
    const lock = LockService.getScriptLock();
    lock.waitLock(5000);
    try {
      const cadastroAtual = obterCadastroPendente(escaninho);
      if (cadastroAtual) {
        cadastrarOuAtualizarCartao(abaCartoes, uid, cadastroAtual.nome, escaninho);
        registrarAcesso(abaAcessos, [new Date(), uid, cadastroAtual.nome, escaninho, "CADASTRADO", "CADASTRO", dispositivo]);
        removerCadastroPendente(escaninho);
        return responderJSON({
          sucesso: true,
          autorizado: true,
          cadastrado: true,
          uid: uid,
          nome: cadastroAtual.nome,
          escaninho: escaninho,
          status: "AUTORIZADO",
          mensagem: "Cartão cadastrado e autorizado"
        });
      }
    } finally {
      lock.releaseLock();
    }
  }
  const resultado = buscarCartao(abaCartoes, uid, escaninho);
  let nome = "Não cadastrado";
  let autorizado = false;
  let status = "DESCONHECIDO";
  if (resultado.exato) {
    nome = resultado.exato.nome;
    if (resultado.exato.ativo) {
      autorizado = true;
      status = "AUTORIZADO";
    } else status = "BLOQUEADO";
  } else if (resultado.outroEscaninho) {
    nome = resultado.outroEscaninho.nome;
    status = "ESCANINHO_INCORRETO";
  }
  registrarAcesso(abaAcessos, [new Date(), uid, nome, escaninho, status, "RFID", dispositivo]);
  return responderJSON({
    sucesso: true,
    autorizado: autorizado,
    cadastrado: false,
    uid: uid,
    nome: nome,
    escaninho: escaninho,
    status: status,
    mensagem: autorizado ? "Acesso autorizado" : "Acesso negado"
  });
}

function processarPINServidor(abaAcessos, abaEscaninhos, dados) {
  const escaninho = normalizarID(dados.escaninho);
  const pin = String(dados.pin || "").trim();
  const dispositivo = String(dados.dispositivo || "");
  if (!/^\d{6}$/.test(pin)) {
    registrarAcesso(abaAcessos, [new Date(), "", "PIN incorreto", escaninho, "NEGADO", "PIN", dispositivo]);
    return responderJSON({sucesso: true, autorizado: false, status: "NEGADO", mensagem: "PIN inválido"});
  }
  const locker = buscarEscaninho(abaEscaninhos, escaninho);
  if (!locker || !locker.ativo || !locker.pinHash || !locker.pinSalt) {
    registrarAcesso(abaAcessos, [new Date(), "", "PIN incorreto", escaninho, "NEGADO", "PIN", dispositivo]);
    return responderJSON({sucesso: true, autorizado: false, status: "NEGADO", mensagem: "PIN não autorizado"});
  }
  const hash = calcularHashPIN(pin, locker.pinSalt, escaninho);
  const autorizado = hash === locker.pinHash;
  registrarAcesso(
      abaAcessos,
      [new Date(), "", autorizado ? "PIN correto" : "PIN incorreto", escaninho, autorizado ? "AUTORIZADO" : "NEGADO", "PIN", dispositivo]);
  return responderJSON({
    sucesso: true,
    autorizado: autorizado,
    status: autorizado ? "AUTORIZADO" : "NEGADO",
    mensagem: autorizado ? "Senha correta" : "Senha incorreta"
  });
}

function iniciarCadastroCartao(nome, escaninho) {
  nome = String(nome || "").trim();
  escaninho = normalizarID(escaninho);
  if (nome.length < 2) throw new Error("Digite o nome.");
  if (!escaninho) throw new Error("Selecione o escaninho.");
  const aba = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(ABA_ESCANINHOS);
  const locker = buscarEscaninho(aba, escaninho);
  if (!locker) throw new Error("Escaninho não encontrado.");
  const agora = Date.now();
  PropertiesService.getScriptProperties().setProperty(
      chaveCadastro(escaninho), JSON.stringify({nome: nome, escaninho: escaninho, expiraEm: agora + TEMPO_CADASTRO_MS}));
  return {sucesso: true, mensagem: "Encoste o cartão no leitor."};
}

function cancelarCadastroCartao(escaninho) {
  removerCadastroPendente(escaninho);
  return {sucesso: true};
}

function chaveCadastro(escaninho) {
  return ("CADASTRO_" + normalizarID(escaninho));
}

function obterCadastroPendente(escaninho) {
  const props = PropertiesService.getScriptProperties();
  const chave = chaveCadastro(escaninho);
  const texto = props.getProperty(chave);
  if (!texto) return null;
  try {
    const dados = JSON.parse(texto);
    if (Date.now() > Number(dados.expiraEm || 0)) {
      props.deleteProperty(chave);
      return null;
    }
    return dados;
  } catch (erro) {
    props.deleteProperty(chave);
    return null;
  }
}

function removerCadastroPendente(escaninho) {
  PropertiesService.getScriptProperties().deleteProperty(chaveCadastro(escaninho));
}

function cadastrarOuAtualizarCartao(aba, uid, nome, escaninho) {
  const ultimaLinha = aba.getLastRow();
  if (ultimaLinha >= 2) {
    const dados = aba.getRange(2, 1, ultimaLinha - 1, 3).getDisplayValues();
    for (let i = 0; i < dados.length; i++) {
      if (normalizarUID(dados[i][0]) === uid && normalizarID(dados[i][2]) === escaninho) {
        aba.getRange(i + 2, 1, 1, 5).setValues([[uid, nome, escaninho, "SIM", new Date()]]);
        return;
      }
    }
  }
  aba.appendRow([uid, nome, escaninho, "SIM", new Date()]);
}

function buscarCartao(aba, uid, escaninho) {
  const resultado = {exato: null, outroEscaninho: null};
  const ultimaLinha = aba.getLastRow();
  if (ultimaLinha < 2) return resultado;
  const dados = aba.getRange(2, 1, ultimaLinha - 1, 5).getDisplayValues();
  for (let i = 0; i < dados.length; i++) {
    const uidLinha = normalizarUID(dados[i][0]);
    if (uidLinha !== uid) continue;
    const cartao =
        {uid: uidLinha, nome: String(dados[i][1] || "").trim(), escaninho: normalizarID(dados[i][2]), ativo: interpretarAtivo(dados[i][3])};
    if (cartao.escaninho === escaninho) {
      resultado.exato = cartao;
      return resultado;
    }
    if (!resultado.outroEscaninho) resultado.outroEscaninho = cartao;
  }
  return resultado;
}

function salvarPinEscaninho(escaninho, pin) {
  escaninho = normalizarID(escaninho);
  pin = String(pin || "").trim();
  if (!/^\d{6}$/.test(pin)) throw new Error("O PIN deve ter exatamente 6 números.");
  const aba = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(ABA_ESCANINHOS);
  const locker = buscarEscaninho(aba, escaninho);
  if (!locker) throw new Error("Escaninho não encontrado.");
  const salt = Utilities.getUuid().replace(/-/g, "");
  const hash = calcularHashPIN(pin, salt, escaninho);
  aba.getRange(locker.linha, 4, 1, 3).setValues([[hash, salt, new Date()]]);
  return {sucesso: true, mensagem: "PIN salvo com sucesso."};
}

function removerPinEscaninho(escaninho) {
  const aba = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(ABA_ESCANINHOS);
  const locker = buscarEscaninho(aba, normalizarID(escaninho));
  if (!locker) throw new Error("Escaninho não encontrado.");
  aba.getRange(locker.linha, 4, 1, 3).clearContent();
  return {sucesso: true, mensagem: "PIN removido."};
}

function calcularHashPIN(pin, salt, escaninho) {
  const pepper = PropertiesService.getScriptProperties().getProperty("PIN_PEPPER");
  const texto = escaninho + "|" + salt + "|" + pin + "|" + pepper;
  const bytes = Utilities.computeDigest(Utilities.DigestAlgorithm.SHA_256, texto, Utilities.Charset.UTF_8);
  return bytes
      .map(function(byte) {
        const valor = byte < 0 ? byte + 256 : byte;
        return ("0" + valor.toString(16)).slice(-2);
      })
      .join("");
}

function buscarEscaninho(aba, id) {
  id = normalizarID(id);
  const ultimaLinha = aba.getLastRow();
  if (ultimaLinha < 2) return null;
  const dados = aba.getRange(2, 1, ultimaLinha - 1, 6).getValues();
  for (let i = 0; i < dados.length; i++) {
    if (normalizarID(dados[i][0]) !== id) continue;
    return {
      linha: i + 2,
      id: id,
      nome: String(dados[i][1] || id),
      ativo: interpretarAtivo(dados[i][2]),
      pinHash: String(dados[i][3] || ""),
      pinSalt: String(dados[i][4] || ""),
      pinAtualizadoEm: dados[i][5]
    };
  }
  return null;
}

function garantirEscaninhoRegistrado(aba, id) {
  if (buscarEscaninho(aba, id)) return;
  aba.appendRow([id, id, "SIM", "", "", ""]);
}

function atualizarDispositivo(aba, dados) {
  const id = normalizarID(dados.dispositivo);
  const escaninho = normalizarID(dados.escaninho);
  if (!id) return;
  const nome = String(dados.nomeDispositivo || id);
  const ip = String(dados.ip || "");
  const rssi = dados.rssi !== undefined ? dados.rssi : "";
  const firmware = String(dados.firmware || "");
  const ultimaLinha = aba.getLastRow();
  if (ultimaLinha >= 2) {
    const ids = aba.getRange(2, 1, ultimaLinha - 1, 1).getDisplayValues();
    for (let i = 0; i < ids.length; i++) {
      if (normalizarID(ids[i][0]) === id) {
        aba.getRange(i + 2, 1, 1, 7).setValues([[id, escaninho, nome, new Date(), ip, rssi, firmware]]);
        return;
      }
    }
  }
  aba.appendRow([id, escaninho, nome, new Date(), ip, rssi, firmware]);
}

function registrarAcesso(aba, linha) {
  const lock = LockService.getScriptLock();
  lock.waitLock(5000);
  try {
    aba.appendRow(linha);
  } finally {
    lock.releaseLock();
  }
}

function alterarStatusCartao(uid, escaninho, ativo) {
  uid = normalizarUID(uid);
  escaninho = normalizarID(escaninho);
  const aba = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(ABA_CARTOES);
  const ultimaLinha = aba.getLastRow();
  if (ultimaLinha < 2) throw new Error("Cartão não encontrado.");
  const dados = aba.getRange(2, 1, ultimaLinha - 1, 3).getDisplayValues();
  for (let i = 0; i < dados.length; i++) {
    if (normalizarUID(dados[i][0]) === uid && normalizarID(dados[i][2]) === escaninho) {
      aba.getRange(i + 2, 4).setValue(ativo ? "SIM" : "NAO");
      return {sucesso: true};
    }
  }
  throw new Error("Cartão não encontrado.");
}

function excluirCartao(uid, escaninho) {
  uid = normalizarUID(uid);
  escaninho = normalizarID(escaninho);
  const aba = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(ABA_CARTOES);
  const ultimaLinha = aba.getLastRow();
  if (ultimaLinha < 2) return {sucesso: false};
  const dados = aba.getRange(2, 1, ultimaLinha - 1, 3).getDisplayValues();
  for (let i = 0; i < dados.length; i++) {
    if (normalizarUID(dados[i][0]) === uid && normalizarID(dados[i][2]) === escaninho) {
      aba.deleteRow(i + 2);
      return {sucesso: true};
    }
  }
  return {sucesso: false};
}

function obterDadosDashboard() {
  const planilha = SpreadsheetApp.getActiveSpreadsheet();
  const fuso = planilha.getSpreadsheetTimeZone() || "America/Sao_Paulo";
  const abaCartoes = planilha.getSheetByName(ABA_CARTOES);
  const abaAcessos = planilha.getSheetByName(ABA_ACESSOS);
  const abaEscaninhos = planilha.getSheetByName(ABA_ESCANINHOS);
  const abaDispositivos = planilha.getSheetByName(ABA_DISPOSITIVOS);
  const cartoes = [];
  let cartoesAtivos = 0;
  if (abaCartoes.getLastRow() >= 2) {
    const dados = abaCartoes.getRange(2, 1, abaCartoes.getLastRow() - 1, 5).getValues();
    dados.forEach(function(linha) {
      if (!String(linha[0] || "").trim()) return;
      const ativo = interpretarAtivo(linha[3]);
      if (ativo) cartoesAtivos++;
      cartoes.push({
        uid: String(linha[0] || ""),
        nome: String(linha[1] || ""),
        escaninho: String(linha[2] || ""),
        ativo: ativo,
        cadastradoEm: formatarData(linha[4], fuso)
      });
    });
  }
  const acessos = [];
  let acessosHoje = 0;
  let autorizadosHoje = 0;
  let negadosHoje = 0;
  const hoje = Utilities.formatDate(new Date(), fuso, "yyyy-MM-dd");
  if (abaAcessos.getLastRow() >= 2) {
    const dados = abaAcessos.getRange(2, 1, abaAcessos.getLastRow() - 1, 7).getValues();
    for (let i = dados.length - 1; i >= 0; i--) {
      const linha = dados[i];
      const status = String(linha[4] || "").toUpperCase();
      let dataComparacao = "";
      if (linha[0] instanceof Date) dataComparacao = Utilities.formatDate(linha[0], fuso, "yyyy-MM-dd");
      if (dataComparacao === hoje && status !== "CADASTRADO") {
        acessosHoje++;
        if (status === "AUTORIZADO") autorizadosHoje++;
        else negadosHoje++;
      }
      if (acessos.length < LIMITE_REGISTROS)
        acessos.push({
          data: formatarData(linha[0], fuso),
          uid: String(linha[1] || ""),
          nome: String(linha[2] || ""),
          escaninho: String(linha[3] || ""),
          status: status,
          metodo: String(linha[5] || ""),
          dispositivo: String(linha[6] || "")
        });
    }
  }
  const mapaDispositivos = {};
  if (abaDispositivos.getLastRow() >= 2) {
    const dados = abaDispositivos.getRange(2, 1, abaDispositivos.getLastRow() - 1, 7).getValues();
    dados.forEach(function(linha) {
      const escaninho = normalizarID(linha[1]);
      let online = false;
      if (linha[3] instanceof Date) online = Date.now() - linha[3].getTime() <= TEMPO_ONLINE_MS;
      mapaDispositivos[escaninho] = {
        id: String(linha[0] || ""),
        online: online,
        ultimaComunicacao: formatarData(linha[3], fuso),
        ip: String(linha[4] || ""),
        rssi: linha[5],
        firmware: String(linha[6] || "")
      };
    });
  }
  const escaninhos = [];
  let escaninhosOnline = 0;
  if (abaEscaninhos.getLastRow() >= 2) {
    const dados = abaEscaninhos.getRange(2, 1, abaEscaninhos.getLastRow() - 1, 6).getValues();
    dados.forEach(function(linha) {
      const id = normalizarID(linha[0]);
      if (!id) return;
      const dispositivo = mapaDispositivos[id] || null;
      if (dispositivo && dispositivo.online) escaninhosOnline++;
      const cadastro = obterCadastroPendente(id);
      escaninhos.push({
        id: id,
        nome: String(linha[1] || id),
        ativo: interpretarAtivo(linha[2]),
        pinConfigurado: Boolean(String(linha[3] || "") && String(linha[4] || "")),
        pinAtualizadoEm: formatarData(linha[5], fuso),
        online: dispositivo ? dispositivo.online : false,
        dispositivo: dispositivo,
        cadastro: cadastro ? {nome: cadastro.nome} : null
      });
    });
  }
  return {
    acessosHoje: acessosHoje,
    autorizadosHoje: autorizadosHoje,
    negadosHoje: negadosHoje,
    cartoesAtivos: cartoesAtivos,
    totalCartoes: cartoes.length,
    escaninhosOnline: escaninhosOnline,
    totalEscaninhos: escaninhos.length,
    cartoes: cartoes,
    acessos: acessos,
    escaninhos: escaninhos,
    atualizadoEm: Utilities.formatDate(new Date(), fuso, "dd/MM/yyyy HH:mm:ss")
  };
}

function interpretarAtivo(valor) {
  const texto = String(valor || "").trim().toUpperCase();
  if (!texto) return true;
  return ["SIM", "S", "TRUE", "1", "ATIVO"].includes(texto);
}

function normalizarUID(valor) {
  return String(valor || "").trim().replace(/[^A-Fa-f0-9]/g, "").toUpperCase();
}

function normalizarID(valor) {
  return String(valor || "").trim().toUpperCase();
}

function formatarData(valor, fuso) {
  if (valor instanceof Date && !isNaN(valor.getTime())) return Utilities.formatDate(valor, fuso, "dd/MM/yyyy HH:mm:ss");
  return "";
}

function responderJSON(objeto) {
  return ContentService.createTextOutput(JSON.stringify(objeto)).setMimeType(ContentService.MimeType.JSON);
}