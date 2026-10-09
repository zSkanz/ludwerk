Quero implementar na Ludwerk uma camada de serviços genéricos de plataforma sobre as integrações específicas de cada plataforma.

IMPORTANTE:
No momento, a ÚNICA integração de plataforma realmente disponível é XboxService / Microsoft GDK.

Portanto:
- Xbox será o único provider/backend funcional nesta primeira implementação.
- NÃO implementar Steam, PlayStation ou Nintendo fictícios.
- NÃO adicionar mocks fingindo suporte a plataformas que ainda não existem.
- Porém, a arquitetura NÃO pode depender diretamente do Xbox de forma que seja necessário reescrever os serviços genéricos quando PlayStation, Nintendo, Steam etc. forem adicionados.
- Xbox deve ser apenas o primeiro provider real da arquitetura.

Antes de modificar qualquer código, analise a arquitetura atual da Ludwerk.

Dê atenção especial a:
- sistema de Services e game:GetService();
- XboxService atual e sua implementação GDK;
- NetworkService;
- sistema de scenes;
- lifecycle da engine/runtime;
- sistema de bindings C++ → Luau;
- enums e tipos expostos ao Luau;
- sistema assíncrono/promises/tasks existente;
- configuração dos projetos;
- sistema de plataformas/módulos que já estiver implementado;
- padrões atuais de erro/Result;
- persistência/configuração;
- CMake/build system.

NÃO crie uma arquitetura paralela se já houver uma infraestrutura adequada.

==================================================
OBJETIVO
==================================================

Adicionar inicialmente estes serviços genéricos:

- IdentityService
- AchievementService
- StoreService
- CloudSaveService
- LeaderboardService
- SocialService

Eles devem fornecer uma API comum para recursos normalmente oferecidos pelas plataformas.

Nesta primeira versão:

Generic Services
        |
        ↓
Xbox Provider
        |
        ↓
XboxService / implementação GDK
        |
        ↓
Microsoft GDK

Futuramente:

Generic Services
        |
        +---- Xbox Provider → GDK
        |
        +---- PlayStation Provider → PlayStation SDK
        |
        +---- Nintendo Provider → Nintendo SDK
        |
        +---- Steam Provider → Steamworks

A adição desses providers no futuro NÃO deve exigir alteração significativa na API Luau dos serviços genéricos.

==================================================
1. PRINCÍPIO ARQUITETURAL
==================================================

Existirão dois níveis de API.

NÍVEL 1 — Generic Services

API recomendada para jogos multiplataforma:

game:GetService("IdentityService")
game:GetService("AchievementService")
game:GetService("StoreService")
game:GetService("CloudSaveService")
game:GetService("LeaderboardService")
game:GetService("SocialService")

NÍVEL 2 — Platform-specific Services

Para funcionalidades exclusivas ou avançadas:

game:GetService("XboxService")

Futuramente poderão existir:

PlayStationService
NintendoService
SteamService

Os serviços genéricos NÃO substituem XboxService.

XboxService continua existindo para funcionalidades específicas do ecossistema Xbox.

==================================================
2. NÃO ACOPLAR GENERIC SERVICES AO XBOX
==================================================

Evitar isto:

AchievementService.cpp:

if (XboxService::IsAvailable())
    XboxService::UnlockAchievement(...);

Isso cria acoplamento direto.

Criar uma abstração interna pequena para providers/capabilities.

Exemplo conceitual:

IAchievementProvider
IIdentityProvider
IStoreProvider
ICloudSaveProvider
ILeaderboardProvider
ISocialProvider

Xbox registra implementações dessas interfaces.

Exemplo:

XboxAchievementProvider
XboxIdentityProvider
XboxStoreProvider
XboxCloudSaveProvider
XboxLeaderboardProvider
XboxSocialProvider

Porém:

NÃO criar abstrações excessivamente complexas.

Não criar dezenas de interfaces, factories, registries e managers se uma solução menor resolver.

A abstração deve existir apenas onde for necessária para permitir providers futuros.

==================================================
3. PLATFORM PROVIDER REGISTRY
==================================================

Criar ou aproveitar uma infraestrutura que permita registrar capabilities/providers.

Conceitualmente:

PlatformServices
    |
    +-- IdentityProvider
    +-- AchievementProvider
    +-- StoreProvider
    +-- CloudSaveProvider
    +-- LeaderboardProvider
    +-- SocialProvider

Na implementação atual:

IdentityProvider       → Xbox
AchievementProvider    → Xbox
StoreProvider          → Xbox
CloudSaveProvider      → Xbox
LeaderboardProvider    → Xbox
SocialProvider         → Xbox

Mas isso NÃO significa que todos necessariamente estarão disponíveis.

O Xbox provider deve declarar suas capabilities reais.

Se determinada funcionalidade ainda não estiver implementada no XboxService/GDK, o Generic Service correspondente deve informar que está indisponível.

Nunca simular sucesso.

==================================================
4. CAPABILITIES
==================================================

Os Generic Services devem permitir descobrir se determinada funcionalidade está disponível.

Exemplo conceitual:

local Achievements = game:GetService("AchievementService")

if Achievements:IsAvailable() then
    Achievements:Unlock("FIRST_WIN")
end

Quando necessário, permitir verificação mais específica:

StoreService:Supports(...)
SocialService:Supports(...)

Não criar Supports() desnecessariamente se IsAvailable() for suficiente.

Use capabilities somente onde existirem funcionalidades opcionais dentro do mesmo serviço.

==================================================
5. IDENTITY SERVICE
==================================================

Criar:

game:GetService("IdentityService")

Responsabilidade:

Representar a identidade do jogador na plataforma atual.

API conceitual:

IdentityService:IsAvailable()

IdentityService:GetLocalUser()

IdentityService:IsSignedIn()

IdentityService:SignInAsync()

O usuário retornado deve utilizar uma representação genérica da Ludwerk.

Exemplo:

local user = IdentityService:GetLocalUser()

if user then
    print(user.Id)
    print(user.DisplayName)
end

Não expor XUID como se fosse um ID universal.

Se for necessário expor identificadores específicos, usar metadata/platform-specific API ou XboxService.

Exemplo conceitual:

Generic User:
Id
DisplayName
IsSignedIn
Platform

Xbox-specific:
XboxService:GetXuid()

Não assumir que todas as plataformas possuem o mesmo modelo de autenticação.

==================================================
6. ACHIEVEMENT SERVICE
==================================================

Criar:

game:GetService("AchievementService")

API inicial conceitual:

AchievementService:IsAvailable()

AchievementService:UnlockAsync(id)

AchievementService:GetProgressAsync(id)

AchievementService:SetProgressAsync(id, progress)

O jogo deve trabalhar com IDs internos da Ludwerk.

Exemplo:

AchievementService:UnlockAsync("FIRST_WIN")

Preparar configuração por projeto para mapear:

Ludwerk Achievement ID
        ↓
Platform Achievement ID

Exemplo:

FIRST_WIN

Xbox:
    <Xbox Achievement ID>

Futuramente:

PlayStation:
    <Trophy ID>

Steam:
    <Achievement ID>

O gameplay não deve precisar mudar quando outra plataforma for adicionada.

==================================================
7. STORE SERVICE
==================================================

Criar:

game:GetService("StoreService")

Responsabilidades genéricas:

- consultar produtos;
- verificar ownership/licença;
- iniciar compras;
- consultar disponibilidade da loja;
- restaurar/atualizar entitlement quando aplicável.

API conceitual:

StoreService:IsAvailable()

StoreService:GetProductAsync(id)

StoreService:PurchaseAsync(id)

StoreService:OwnsAsync(id)

StoreService:RefreshEntitlementsAsync()

Usar IDs internos do projeto.

Exemplo:

StoreService:PurchaseAsync("DLC_01")

Mapeamento:

DLC_01
    ↓
Xbox Product/Store ID

Futuramente o mesmo ID pode mapear para:
PlayStation Product ID
Nintendo Product ID
Steam App/DLC ID

Não armazenar credenciais sensíveis no projeto exportado.

Não simular compra bem-sucedida quando StoreService estiver indisponível.

==================================================
8. CLOUD SAVE SERVICE
==================================================

Criar:

game:GetService("CloudSaveService")

NÃO substituir filesystem local.

Separar claramente:

filesystem/local save
        ≠
CloudSaveService

API conceitual:

CloudSaveService:IsAvailable()

CloudSaveService:WriteAsync(slot, data)

CloudSaveService:ReadAsync(slot)

CloudSaveService:DeleteAsync(slot)

CloudSaveService:ExistsAsync(slot)

Considerar:
- usuário atual;
- sincronização;
- conflitos;
- falhas de rede;
- quota/limites da plataforma;
- operação offline quando aplicável.

Não assumir que todas as plataformas implementam cloud save da mesma forma.

Criar um contrato genérico suficientemente pequeno.

==================================================
9. LEADERBOARD SERVICE
==================================================

Criar:

game:GetService("LeaderboardService")

API conceitual:

LeaderboardService:IsAvailable()

LeaderboardService:SubmitScoreAsync(
    "HIGH_SCORE",
    score
)

LeaderboardService:GetTopAsync(
    "HIGH_SCORE",
    count
)

LeaderboardService:GetAroundPlayerAsync(
    "HIGH_SCORE",
    count
)

Usar IDs internos da Ludwerk.

Mapear posteriormente para IDs da plataforma.

Não assumir que todos os providers oferecem exatamente os mesmos filtros ou consultas.

Funcionalidades específicas continuam disponíveis através do serviço nativo da plataforma.

==================================================
10. SOCIAL SERVICE
==================================================

Criar:

game:GetService("SocialService")

Responsabilidades genéricas possíveis:

- amigos;
- presença;
- informações sociais básicas;
- bloqueios;
- possibilidade de comunicação;
- perfil quando houver equivalência razoável.

API inicial conceitual:

SocialService:IsAvailable()

SocialService:GetFriendsAsync()

SocialService:GetPresenceAsync(userId)

SocialService:IsBlockedAsync(userId)

SocialService:CanCommunicateWithAsync(userId)

Não tentar universalizar APIs sociais muito específicas do Xbox.

Funcionalidades sem equivalente genérico permanecem no XboxService.

==================================================
11. XBOX SERVICE
==================================================

XboxService continua sendo a API específica Xbox.

Não remover funcionalidades existentes.

Não quebrar código atual.

Funcionalidades genuinamente Xbox podem permanecer nele.

Exemplos conceituais:

XboxService:IsAvailable()

XboxService:GetUser()

XboxService:GetXuid()

XboxService:CheckPrivilege(...)

XboxService:ResolvePrivilege(...)

XboxService:SetMultiplayerActivity(...)

XboxService:ShowInviteUI(...)

etc.

Se uma funcionalidade do Xbox também alimentar um Generic Service, reutilizar internamente a implementação.

Não implementar a mesma lógica duas vezes.

Exemplo:

AchievementService
       ↓
XboxAchievementProvider
       ↓
implementação compartilhada GDK

XboxService também pode acessar essa mesma camada quando necessário.

==================================================
12. NÃO DUPLICAR NETWORKSERVICE
==================================================

A Ludwerk já possui NetworkService.

NÃO criar MultiplayerService nesta etapa.

NÃO mover P2P, relay, sockets, replicação, host/client ou RemoteEvents para os novos serviços.

NetworkService continua responsável pela infraestrutura de rede existente.

XboxService pode fornecer integração Xbox relacionada a:
- privileges;
- activity;
- invites;
- integração social necessária;
- entrada em sessões Xbox quando aplicável.

Se futuramente for necessário criar uma abstração genérica de sessões de plataforma, isso será avaliado separadamente.

Não duplicar NetworkService agora.

==================================================
13. LIFECYCLE DOS PROVIDERS
==================================================

SDKs de plataforma NÃO devem depender do lifetime de uma Scene.

Trocar de Scene não deve reinicializar autenticação ou integração Xbox.

Separar:

Engine/runtime lifetime
        ↓
Platform providers
        ↓
Services Luau

Os providers devem possuir lifecycle apropriado ao runtime da aplicação.

game:GetService() fornece acesso ao serviço, mas não deve significar necessariamente que o SDK foi inicializado naquele momento.

==================================================
14. COMPORTAMENTO QUANDO INDISPONÍVEL
==================================================

Todos os Generic Services devem existir mesmo quando não houver provider ativo.

Exemplo:

local Store = game:GetService("StoreService")

print(Store:IsAvailable())
-- false

Não retornar nil em GetService apenas porque o provider está ausente.

Operações indisponíveis devem retornar erro/Result explícito.

Exemplo conceitual:

IntegrationUnavailable
ProviderUnavailable
NotSignedIn
PermissionDenied
NetworkError
NotSupported
InvalidProduct
etc.

Operações async NÃO podem ficar esperando indefinidamente porque o provider está ausente.

Nunca simular sucesso.

==================================================
15. RESULTADOS E ERROS
==================================================

Antes de criar um novo sistema de Result/Error, verificar o padrão existente na Ludwerk.

Reutilizar o padrão atual se adequado.

Os erros genéricos não devem expor códigos GDK diretamente como contrato público.

Entretanto, permitir diagnóstico avançado.

Conceitualmente:

result.Success
result.Error
result.Message

e opcionalmente:

result.PlatformError

PlatformError pode conter informação específica útil para debug sem tornar o gameplay dependente dela.

==================================================
16. ASYNC
==================================================

Integrações de plataforma possuem muitas operações assíncronas.

Reutilizar o sistema async existente da Ludwerk.

Não inventar uma segunda implementação de promises/futures/tasks se já existir uma.

As APIs Luau devem seguir o padrão atual da engine.

==================================================
17. CONFIGURAÇÃO NO EDITOR
==================================================

Adicionar configuração apropriada por projeto para os Generic Services.

Exemplo conceitual:

Platforms & Integrations

Xbox
[x] Enabled

Generic Services

Identity
Provider: Xbox

Achievements
Provider: Xbox

Store
Provider: Xbox

Cloud Saves
Provider: Xbox

Leaderboards
Provider: Xbox

Social
Provider: Xbox

Como Xbox é o único provider atualmente, não é necessário criar uma UI complexa de seleção.

Porém, internamente não hardcode:

Provider = Xbox forever.

Preparar estrutura de configuração que futuramente possa aceitar:

Auto
Xbox
PlayStation
Nintendo
Steam
etc.

==================================================
18. AUTO PROVIDER
==================================================

Preferencialmente suportar conceitualmente:

Provider = Auto

Nesse modo a Ludwerk escolhe o provider disponível para o target atual.

Hoje:

Xbox target / GDK integration
        ↓
Xbox Provider

No futuro:

PlayStation target
        ↓
PlayStation Provider

Steam build
        ↓
Steam Provider

Não implementar detecção de plataformas inexistentes agora.

Apenas preparar o contrato/configuração para isso.

==================================================
19. MAPEAMENTO DE IDS
==================================================

AchievementService, StoreService e LeaderboardService devem utilizar IDs internos do projeto.

Exemplo:

FIRST_WIN
DLC_01
GLOBAL_SCORE

Criar configuração que permita mapear esses IDs para cada provider.

Conceitualmente:

FIRST_WIN:
    Xbox: "..."

DLC_01:
    Xbox: "..."

GLOBAL_SCORE:
    Xbox: "..."

A estrutura deve permitir futuramente:

FIRST_WIN:
    Xbox: "..."
    PlayStation: "..."
    Steam: "..."

Sem alterar scripts Luau.

==================================================
20. API PÚBLICA NÃO PODE VAZAR GDK
==================================================

Muito importante:

Generic Services NÃO devem expor:
- tipos GDK;
- headers GDK;
- enums GDK;
- handles GDK;
- structs GDK;
- códigos GDK como API principal.

Criar tipos próprios da Ludwerk.

A cadeia deve ser:

Luau
 ↓
Ludwerk Generic API
 ↓
Provider Interface
 ↓
Xbox Provider
 ↓
XboxService/GDK integration
 ↓
GDK

Isso permitirá adicionar outros SDKs posteriormente.

==================================================
21. OPEN SOURCE / SDK PROPRIETÁRIO
==================================================

A Ludwerk é open source.

Manter isolamento entre:
- código público da engine;
- adapters/providers;
- SDKs proprietários.

Não copiar nem redistribuir componentes proprietários que não possam fazer parte do repositório.

O código deve continuar compilável quando GDK não estiver instalado, de acordo com a arquitetura de módulos/plataformas existente.

==================================================
22. PREPARAÇÃO PARA PROVIDERS FUTUROS
==================================================

NÃO implementar estes agora:

PlayStationProvider
NintendoProvider
SteamProvider

Mas validar arquiteturalmente que futuramente seja possível adicionar, por exemplo:

class PlayStationAchievementProvider
    : public IAchievementProvider

class SteamAchievementProvider
    : public IAchievementProvider

sem modificar AchievementService.

O mesmo vale para:

Identity
Store
CloudSave
Leaderboard
Social

Adicionar um novo provider deve envolver principalmente:
1. implementar interfaces/capabilities;
2. registrar provider;
3. adicionar configuração/mapeamentos;
4. integrar SDK;
5. testar.

Não reescrever Generic Services.

==================================================
23. EVITAR OVERENGINEERING
==================================================

Essa regra é importante.

Queremos extensibilidade, NÃO uma arquitetura enterprise gigantesca.

Não criar:
- dezenas de managers;
- factories desnecessárias;
- service locators duplicados;
- dependency injection framework;
- interfaces de uma única função sem benefício;
- abstrações para plataformas que nem temos acesso.

Criar a menor arquitetura que permita:

Generic Service
      ↓
Provider contract
      ↓
Xbox implementation

e que futuramente permita adicionar outro provider.

==================================================
24. DOCUMENTAÇÃO LUAU
==================================================

Atualizar os tipos/documentação Luau.

Os novos serviços devem aparecer corretamente em:

game:GetService()

autocomplete/intellisense da Ludwerk.

Documentar:
- métodos;
- propriedades;
- eventos;
- resultados;
- erros;
- disponibilidade;
- comportamento async.

Não expor uma API como funcional se o backend Xbox correspondente ainda não estiver implementado.

==================================================
25. TESTES
==================================================

Criar testes para pelo menos:

- serviço disponível;
- serviço indisponível;
- provider registrado;
- provider ausente;
- usuário autenticado;
- usuário não autenticado;
- operação async falhando;
- IDs inexistentes;
- provider retornando erro;
- mudança de Scene sem destruir provider;
- GDK ausente;
- Xbox integration desabilitada;
- projeto sem Xbox habilitado.

Onde APIs GDK reais não puderem ser executadas em CI, testar a camada genérica através de test providers internos.

IMPORTANTE:
Test providers são para testes automatizados, NÃO devem aparecer como plataformas/providers disponíveis ao desenvolvedor.

==================================================
26. COMPATIBILIDADE
==================================================

Não quebrar:
- XboxService atual;
- NetworkService;
- InputService;
- SceneService;
- projetos existentes;
- exportação Windows/Linux/Android;
- builds sem GDK.

Se alguma alteração incompatível for realmente necessária, documentar antes de executá-la e procurar uma alternativa compatível.

==================================================
27. ORDEM DE IMPLEMENTAÇÃO
==================================================

Executar incrementalmente.

FASE 1
Analisar código atual.

Produzir plano mostrando:
- arquivos existentes relevantes;
- arquitetura atual do XboxService;
- lifecycle;
- bindings Luau;
- async;
- configuração;
- como providers serão encaixados.

FASE 2
Criar infraestrutura mínima de providers/capabilities.

FASE 3
Implementar IdentityService + XboxIdentityProvider.

Compilar e testar.

FASE 4
Implementar AchievementService + XboxAchievementProvider.

Compilar e testar.

FASE 5
Implementar StoreService + XboxStoreProvider.

Compilar e testar.

FASE 6
Implementar CloudSaveService + XboxCloudSaveProvider.

Compilar e testar.

FASE 7
Implementar LeaderboardService + XboxLeaderboardProvider.

Compilar e testar.

FASE 8
Implementar Social