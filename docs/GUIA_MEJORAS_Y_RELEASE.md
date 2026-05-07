# Guia practica: mejoras del plugin y build estable

## 1) Que significa "workflow oficial"

El workflow oficial es el pipeline de referencia para producir el artefacto final (Stobe.dll) que consideras valido para usar o distribuir.

En este repo, el workflow oficial es:

- [.github/workflows/build-windows.yml](.github/workflows/build-windows.yml)

Ese workflow:

1. Prepara toolchain MSVC v100 en runner Windows.
2. Prepara SDK bloqueado (KenshiLib + Boost 1.60.0 + libs requeridas).
3. Compila Stobe.dll.
4. Sube artefacto descargable.

Regla simple:

- Si este workflow pasa en verde, tu mejora esta "build-ok".

## 2) Flujo exacto cuando terminas una mejora

Haz esto siempre, en este orden:

1. Crea una rama de trabajo nueva desde stobe.
2. Implementa tu mejora.
3. Commit pequeno y claro.
4. Push de la rama.
5. Abre Pull Request hacia stobe.
6. Espera workflow verde.
7. Si todo bien, merge.
8. Descarga el artefacto del run verde.

Comandos recomendados:

  git checkout stobe
  git pull
  git checkout -b feat/nombre-corto
  git add .
  git commit -m "feat: descripcion corta"
  git push -u origin feat/nombre-corto

Despues del merge (si trabajas directo en stobe tambien aplica):

  gh run list --workflow build-windows.yml --limit 5
  gh run view <RUN_ID> --log

## 3) Como saber rapido si una mejora quedo bien

Checklist minimo:

1. Run status: success.
2. Paso Build plugin: success.
3. Mensaje final: Built DLL.
4. Paso Upload artifacts: success.

Si esos cuatro puntos se cumplen, el build esta correcto.

## 4) Control de versiones recomendado (sin perder el original)

Usa 3 capas de seguridad:

1. Rama estable
- Mantener stobe como rama estable publicable.

2. Ramas por feature
- Una rama por mejora: feat/*, fix/*, refactor/*.

3. Tags de hitos
- Crear tag cada vez que tengas una version que funciona en juego.

Ejemplo:

  git checkout stobe
  git pull
  git tag -a v0.8.8-stable -m "build estable con CI v100"
  git push origin v0.8.8-stable

Con eso puedes volver en segundos a un estado conocido.

## 5) Como volver al "original" cuando quieras

Opciones practicas:

1. Volver a una version etiquetada (recomendado)

  git checkout v0.8.8-stable

2. Crear rama nueva desde una tag para seguir trabajando desde ahi

  git checkout -b hotfix/desdev088 v0.8.8-stable

3. Restaurar stobe al estado de un commit concreto

  git checkout stobe
  git reset --hard <COMMIT_BUENO>
  git push --force-with-lease

Nota: el punto 3 reescribe historia remota. Usalo solo si estas seguro.

## 6) Workflow rapido vs workflow oficial

Ahora este repo tiene ambos:

1. Workflow rapido
- [.github/workflows/build-quick.yml](.github/workflows/build-quick.yml)
- Objetivo: feedback rapido de compilacion (valida cambios de codigo antes de release).
- Usa Visual Studio 2022 (v143), no instala MSVC v100, no publica artefacto de release.

2. Workflow oficial
- [.github/workflows/build-windows.yml](.github/workflows/build-windows.yml)
- Objetivo: build final compatible (v100) + artefacto descargable.

Regla exacta de uso:

1. Mientras desarrollas una mejora
- Mira primero el resultado del workflow rapido.

2. Cuando la mejora esta lista para integrar/publicar
- Exige workflow oficial en verde.

3. Si rapido verde pero oficial rojo
- No liberar. Hay diferencia de toolchain/entorno que debes corregir.

4. Si oficial verde
- Es build valida para usar/distribuir.

## 7) Regla de oro para trabajar tranquilo

1. Nunca trabajar directo sin rama si la mejora es grande.
2. Nunca mergear con CI en rojo.
3. Etiquetar cada hito jugable.
4. Guardar URL del artefacto del ultimo run verde.

Con estas 4 reglas, siempre puedes avanzar y tambien volver atras sin riesgo.