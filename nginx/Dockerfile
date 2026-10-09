FROM nginx:1.27-alpine

COPY default.conf.template /etc/nginx/templates/default.conf.template
COPY 10-validate-deployment-env.sh /docker-entrypoint.d/10-validate-deployment-env.sh

EXPOSE 8443
CMD ["nginx", "-g", "daemon off;"]
